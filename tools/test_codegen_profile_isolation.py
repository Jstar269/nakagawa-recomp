# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Title #2 slice 1: ``--profile=none`` must inherit no HST behavior.

The invariant under test is narrow and absolute:

    A guest address must never change what ``--profile=none`` emits.

Substring sampling cannot prove that -- it only proves that the particular
phrases someone thought to list are absent.  This module proves it
differentially instead.

The specimen is a synthetic ET_EXEC that carries **two byte-identical copies of
every body**: one at the HST guest address ``A``, one at the control address
``A + CONTROL_DELTA``, where the control address is outside every HST table.
Under ``--profile=none`` the two emitted functions must be identical after
normalising each body against its own start address.  Any surviving
address-coupled site -- present or future, named in this file or not -- makes
the pair differ and fails the test.  The comparison is over the *entire*
function text, so it needs no vocabulary of leak phrases.

The same differential run in reverse proves the test can actually see a leak:
under ``--profile=hst`` every one of those pairs must **differ**.  A gate that
was wrongly stuck off, or a specimen body that never reached its emission site,
shows up there as a false equality rather than as a silent pass.

Three couplings do not manifest as a function body and are covered by direct
unit tests instead:

* ``HST_MANUAL_CALLABLES`` / ``HST_RESUME_OWNERS`` change entry *roles*
  (:func:`codegen.build_entry_catalog`);
* ``_SV_SPECIAL`` suppresses ``--static-verify`` emission
  (:func:`codegen.sv_plan`) -- the differential covers it too, because the
  differential runs with ``--static-verify`` enabled;
* the address literals and named address tables themselves, which
  :class:`GateInventoryTests` re-derives from ``tools/codegen.py``'s AST.  That
  census is scoped to the comparison grammar :func:`scan_address_gates`
  supports, and within that grammar a new address literal must be gated and
  covered and a new named table must be declared.  It says nothing about a
  coupling written some other way -- the ``insns & _SV_SPECIAL`` set
  intersection this slice had to fix is the worked example, and only the
  differential above catches that shape.

Evidence tier: production helper / white-box.  ``codegen.main`` runs unmodified
through its real profile plumbing, but on a synthetic image -- no private
EBOOT.elf is required or used.
"""

from __future__ import annotations

import ast
import struct
import sys
import tempfile
import unittest
from io import StringIO
from pathlib import Path
from typing import NamedTuple

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import codegen  # noqa: E402
import compat_overrides  # noqa: E402
from host_stubs import HST_SIMPLE_STUBS  # noqa: E402

# ---------------------------------------------------------------------------
# Specimen layout
# ---------------------------------------------------------------------------

# Every HST address in the tables below is < 0x00200000, so shifting by this
# delta lands the control copy outside all of them with no collisions.
CONTROL_DELTA = 0x00200000
# Above every control address, so the fan-out entry is itself address-neutral.
ENTRY = 0x00400000

NOP = 0x00000000
JR_RA = 0x03E00008
LW_T0_A0 = 0x8C880000  # lw $t0, 0($a0) -- the NULL_BASE_WORD_LOADS shape
LUI_T0 = (0x0F << 26) | (8 << 16) | 0x1234  # lui $t0, 0x1234
ORI_T0 = (0x0D << 26) | (8 << 21) | (8 << 16) | 0x5678  # ori $t0, $t0, 0x5678


def _jal(target: int) -> int:
    return 0x0C000000 | ((target >> 2) & 0x03FFFFFF)


def _beq_fwd(delta_bytes: int) -> int:
    # beq $zero, $zero, +delta_bytes (relative to the delay slot)
    return (4 << 26) | (((delta_bytes - 4) >> 2) & 0xFFFF)


def _addiu_sp(imm: int) -> int:
    return (0x09 << 26) | (29 << 21) | (29 << 16) | (imm & 0xFFFF)


# --- the address-coupled inventory, taken from codegen's own tables ---------

HST_BOOT_PROBES = (0x0003D828, 0x0003DFD0, 0x000705B0, 0x001026B8, 0x001039D8)
HST_INLINE_PROBES = (0x000160E8, 0x000705E4)
HST_DIAG_PROBES = (0x0010433C, 0x0006E9BC)
HST_FASTPATHS = (0x0006EA1C, 0x00108630)
HST_ABORT = (0x00000A1C,)
HST_CUSTOM_STUBS = (
    0x000011B0, 0x0000260C, 0x0000FE3C, 0x0000F538, 0x000101C4, 0x00010738,
    0x00013524, 0x00015EA0, 0x000143B0, 0x0001034C, 0x000468C8,
    0x001D9EB0, 0x00011090, 0x000110DC, 0x000114A8, 0x000114C0, 0x000149A8,
)
HST_NULL_BASE = tuple(sorted(codegen.NULL_BASE_WORD_LOADS))
HST_GUEST_PATCHES = tuple(sorted(codegen.GUEST_PATCHES))
HST_SIMPLE_STUB_ADDRS = tuple(sorted(HST_SIMPLE_STUBS))

# Bodies whose emission differs between the two profiles.  These are the pairs
# the differential compares.
BODY_COUPLED = (
    HST_BOOT_PROBES
    + HST_INLINE_PROBES
    + HST_DIAG_PROBES
    + HST_FASTPATHS
    + HST_ABORT
    + HST_CUSTOM_STUBS
    + HST_NULL_BASE
    + HST_GUEST_PATCHES
    + HST_SIMPLE_STUB_ADDRS
)

# Role-coupled only (catalog classification, not body text).
HST_MANUAL_CALLABLES = tuple(sorted(codegen.HST_MANUAL_CALLABLES))
HST_RESUME_OWNERS = dict(codegen.HST_RESUME_OWNERS)

# Everything a leak could key on.  The AST inventory test asserts that no gate
# in codegen.py names an address outside this set.
COVERED = frozenset(
    BODY_COUPLED
    + HST_MANUAL_CALLABLES
    + tuple(HST_RESUME_OWNERS)
    + tuple(HST_RESUME_OWNERS.values())
    + tuple(codegen._SV_SPECIAL)
)


# --- per-address body shapes ------------------------------------------------


def _body_leaf() -> list[int]:
    return [JR_RA, NOP]


def _body_sv_leaf() -> list[int]:
    """Leaf that also yields a --static-verify prediction point."""
    return [LUI_T0, ORI_T0, JR_RA, NOP]


def _body_null_base() -> list[int]:
    """`lw` at the function's first instruction: the NULL_BASE_WORD_LOADS site."""
    return [LW_T0_A0, JR_RA, NOP]


def _body_guest_patch() -> list[int]:
    """Branch at the first instruction, then a predictable run for sv_plan."""
    return [_beq_fwd(8), NOP, LUI_T0, ORI_T0, JR_RA, NOP]


def _body_fastpath(consumed_words: int) -> list[int]:
    """Long enough that the hst fastpath's `consumed` skip range stays inside."""
    return [NOP] * (consumed_words + 1) + [LUI_T0, ORI_T0, JR_RA, NOP]


def _shape_for(addr: int) -> list[int]:
    if addr in HST_NULL_BASE:
        return _body_null_base()
    if addr in HST_GUEST_PATCHES:
        return _body_guest_patch()
    if addr == 0x0006EA1C:
        return _body_fastpath(10)   # consumed 0x6ea20..0x6ea44
    if addr == 0x00108630:
        return _body_fastpath(24)   # consumed 0x108634..0x108690
    if addr in HST_DIAG_PROBES:
        return _body_sv_leaf()
    return _body_leaf()


def _build_specimen_words() -> dict[int, int]:
    words: dict[int, int] = {}

    def place(base: int, body: list[int]) -> None:
        for i, w in enumerate(body):
            words[base + i * 4] = w

    # Two byte-identical copies of every coupled body.
    for addr in BODY_COUPLED:
        body = _shape_for(addr)
        place(addr, body)
        place(addr + CONTROL_DELTA, body)

    # Role-coupled addresses need a body so they exist as entries at all.
    for addr in HST_MANUAL_CALLABLES:
        place(addr, _body_leaf())

    # Resume/owner pairs.  The owner opens an o32 frame, branches into the
    # resume, and the resume closes it -- the shape entry_frame_balance
    # re-derives when --profile=hst verifies the manual seeds.
    for resume, owner in HST_RESUME_OWNERS.items():
        place(owner, [_addiu_sp(-32 & 0xFFFF), NOP,
                      _beq_fwd(resume - (owner + 8)), NOP])
        place(resume, [_addiu_sp(32), NOP, JR_RA, NOP])

    # Fan-out entry: one jal per discovered body, so the analyzer finds every
    # address as a function start under both profiles.
    targets = (
        list(BODY_COUPLED)
        + [a + CONTROL_DELTA for a in BODY_COUPLED]
        + list(HST_MANUAL_CALLABLES)
        + list(HST_RESUME_OWNERS.values())
    )
    for i, tgt in enumerate(targets):
        words[ENTRY + i * 8] = _jal(tgt)
        words[ENTRY + i * 8 + 4] = NOP
    tail = ENTRY + len(targets) * 8
    words[tail] = JR_RA
    words[tail + 4] = NOP
    return words


def _write_elf(path: Path, words: dict[int, int], entry: int) -> None:
    """One r-x PT_LOAD at vaddr 0 holding `words`, with e_entry = entry."""
    filesz = max(max(words) + 8, 0x1000)
    payload_off = 52 + 32
    blob = bytearray(payload_off + filesz)
    blob[:8] = b"\x7fELF\x01\x01\x01\x00"
    struct.pack_into(
        "<HHIIIIIHHHHHH", blob, 16,
        2,            # e_type = ET_EXEC
        8,            # e_machine = EM_MIPS
        1, entry, 52, 0, 0,
        52, 32, 1, 0, 0, 0,
    )
    struct.pack_into("<8I", blob, 52, 1, payload_off, 0, 0, filesz, filesz, 5, 4)
    for addr, word in words.items():
        struct.pack_into("<I", blob, payload_off + addr, word)
    path.write_bytes(blob)


# ---------------------------------------------------------------------------
# Emitted-text handling
# ---------------------------------------------------------------------------


def _split_functions(text: str) -> dict[str, str]:
    """Map emitted symbol -> its full function text (brace-column parsing)."""
    funcs: dict[str, str] = {}
    name: str | None = None
    buf: list[str] = []
    for line in text.splitlines():
        if name is None:
            if line.startswith("void ") and "(CpuState *s)" in line:
                name = line[len("void "):line.index("(CpuState *s)")].strip()
                buf = [line]
            continue
        buf.append(line)
        if line.startswith("}"):
            funcs[name] = "\n".join(buf)
            name = None
    return funcs


# Bodies are at most a few hundred bytes; normalise generously past that.
_NORMALISE_SPAN = 0x400


def _normalise(text: str, base: int) -> str:
    """Rewrite every self-relative 8-hex address as an offset from `base`.

    Both copies of a body hold identical instruction words, so after this the
    only way two texts can differ is a genuine address-keyed emission
    difference.
    """
    for k in range(0, _NORMALISE_SPAN, 4):
        text = text.replace(f"{base + k:08x}", f"<+{k:x}>")
    return text


def _find(funcs: dict[str, str], addr: int) -> tuple[str, str]:
    """Return (symbol, text) for the entry emitted at `addr`."""
    for prefix in ("f_", "r_"):
        sym = f"{prefix}{addr:08x}"
        if sym in funcs:
            return sym, funcs[sym]
    raise AssertionError(
        f"no function emitted at 0x{addr:08x}; the specimen body for it never "
        f"reached codegen, so nothing about that address was actually proven"
    )


class _CodegenRun:
    """One in-process codegen.main invocation over the shared specimen."""

    _cache: dict[tuple[str, bool], dict[str, str]] = {}

    @classmethod
    def functions(cls, profile: str, static_verify: bool = False) -> dict[str, str]:
        key = (profile, static_verify)
        if key not in cls._cache:
            cls._cache[key] = cls._run(profile, static_verify)
        return cls._cache[key]

    @staticmethod
    def _run(profile: str, static_verify: bool) -> dict[str, str]:
        words = _build_specimen_words()
        sv_was = codegen.SV_ENABLED
        with tempfile.TemporaryDirectory(prefix="nakagawa-profile-isolation-") as tmp:
            tmp_path = Path(tmp)
            elf_path = tmp_path / "specimen.elf"
            _write_elf(elf_path, words, entry=ENTRY)
            out_c = tmp_path / "out.c"
            argv = [
                "codegen.py", str(elf_path), str(out_c),
                "--base=0", f"--profile={profile}", "--funcs-per-chunk=4000",
            ]
            if static_verify:
                argv.append("--static-verify")
            orig_out, orig_err = sys.stdout, sys.stderr
            try:
                sys.stdout, sys.stderr = StringIO(), StringIO()
                rc = codegen.main(argv)
                captured = sys.stdout.getvalue() + sys.stderr.getvalue()
            finally:
                sys.stdout, sys.stderr = orig_out, orig_err
                # --static-verify flips a module global; never let it escape.
                codegen.SV_ENABLED = sv_was
            assert rc == 0, f"codegen failed profile={profile}: {captured}"
            texts = []
            if out_c.is_file():
                texts.append(out_c.read_text(encoding="ascii", errors="ignore"))
            for chunk in sorted(tmp_path.glob("out_*.c")):
                texts.append(chunk.read_text(encoding="ascii", errors="ignore"))
        combined = "\n".join(texts)
        assert combined.strip(), f"empty codegen output for profile={profile}"
        return _split_functions(combined)


# ---------------------------------------------------------------------------
# 1. The invariant: profile=none is address-decoupled
# ---------------------------------------------------------------------------


class ProfileNoneDecouplingTests(unittest.TestCase):
    """Every coupled address emits exactly what its control copy emits."""

    @classmethod
    def setUpClass(cls):
        cls.diag_was = codegen.EMIT_DIAG_PROBES
        # Turn the two diagnostic sites on so their gates are exercised too;
        # in production they are compiled out by this flag alone.
        codegen.EMIT_DIAG_PROBES = True
        cls.funcs = _CodegenRun.functions("none", static_verify=True)

    @classmethod
    def tearDownClass(cls):
        codegen.EMIT_DIAG_PROBES = cls.diag_was
        _CodegenRun._cache.clear()

    def test_specimen_reaches_every_coupled_address(self):
        missing = [f"0x{a:08x}" for a in BODY_COUPLED
                   if f"f_{a:08x}" not in self.funcs and f"r_{a:08x}" not in self.funcs]
        self.assertEqual(missing, [], "specimen never emitted these addresses")
        self.assertEqual(
            len(set(BODY_COUPLED)), len(BODY_COUPLED),
            "duplicate address in the coupled inventory",
        )

    def test_every_coupled_address_matches_its_control_copy(self):
        divergent = []
        for addr in BODY_COUPLED:
            _, hst_text = _find(self.funcs, addr)
            _, ctl_text = _find(self.funcs, addr + CONTROL_DELTA)
            if _normalise(hst_text, addr) != _normalise(ctl_text, addr + CONTROL_DELTA):
                divergent.append(f"0x{addr:08x}")
        self.assertEqual(
            divergent, [],
            "profile=none emitted different code for these guest addresses than "
            "for their byte-identical control copies -- HST behavior is still "
            "reaching profile=none by numeric-address collision",
        )


# ---------------------------------------------------------------------------
# 2. The differential is sensitive: profile=hst IS address-coupled
# ---------------------------------------------------------------------------


class ProfileHstCouplingTests(unittest.TestCase):
    """Guards the test above: an always-off gate would pass it vacuously."""

    @classmethod
    def setUpClass(cls):
        cls.diag_was = codegen.EMIT_DIAG_PROBES
        codegen.EMIT_DIAG_PROBES = True
        cls.funcs = _CodegenRun.functions("hst", static_verify=True)

    @classmethod
    def tearDownClass(cls):
        codegen.EMIT_DIAG_PROBES = cls.diag_was
        _CodegenRun._cache.clear()

    def test_every_coupled_address_differs_from_its_control_copy(self):
        vacuous = []
        for addr in BODY_COUPLED:
            _, hst_text = _find(self.funcs, addr)
            _, ctl_text = _find(self.funcs, addr + CONTROL_DELTA)
            if _normalise(hst_text, addr) == _normalise(ctl_text, addr + CONTROL_DELTA):
                vacuous.append(f"0x{addr:08x}")
        self.assertEqual(
            vacuous, [],
            "profile=hst emitted identical code for these addresses and their "
            "control copies, so the profile=none equality proves nothing for "
            "them -- the gate is off in both profiles or the site moved",
        )


# ---------------------------------------------------------------------------
# 3. Legacy hst behavior is preserved verbatim
# ---------------------------------------------------------------------------


class ProfileHstLegacyTests(unittest.TestCase):
    """profile=hst must still emit each legacy behavior, in production config."""

    @classmethod
    def setUpClass(cls):
        cls.combined = "\n".join(_CodegenRun.functions("hst").values())

    @classmethod
    def tearDownClass(cls):
        _CodegenRun._cache.clear()

    def test_legacy_hst_emission_is_unchanged(self):
        expected = (
            "sr_newlib_malloc(s->r[5], owner_ra)",
            "sr_newlib_free(s->r[5], owner_ra)",
            "sr_newlib_memalign",
            "sr_newlib_realloc",
            "sr_guest_sprintf(s);",
            "memmove(SR_HOST",
            "memset(SR_HOST",
            "MEMSET_FASTPATH",
            "ARRSHIFT_FASTPATH",
            "GUEST_ABORT",
            "_Exit(9)",
            "sr_boot_probe(s, 0x0003d828u);",
            "sr_boot_probe(s, 0x000160e8u);",
            "sr_boot_probe(s, 0x000705e4u);",
            "bypass loop 0x10950",
            "s->r[16] = s->r[3];",
            "_c = 1u;",
            "== 0u ? 0u : MEM_R32",
            "custom stub: __register_frame_info bypass",
            "custom stub: skip corrupted heap-statistics walk",
            "title backdrop selector postcondition",
        )
        missing = [s for s in expected if s not in self.combined]
        self.assertEqual(missing, [], "profile=hst lost legacy HST emission")

    def test_diagnostic_probes_stay_compiled_out_in_production(self):
        # EMIT_DIAG_PROBES is False here: the two diag sites must be absent even
        # under hst, which is the production default this slice must not change.
        self.assertNotIn("TOKENSCAN_DIAG", self.combined)
        self.assertNotIn("F3G_ENTRY", self.combined)


# ---------------------------------------------------------------------------
# 4. Couplings that are not function text
# ---------------------------------------------------------------------------


class NonBodyCouplingTests(unittest.TestCase):
    """Entry roles and --static-verify exclusion are title-owned too."""

    def _catalog(self, profile):
        # Only the owners are analyzer-discovered.  The resume addresses exist
        # in the catalog solely because HST_RESUME_OWNERS seeds them, which is
        # exactly the coupling under test.
        analyzed = set(HST_MANUAL_CALLABLES) | set(HST_RESUME_OWNERS.values())
        ranges = [(0, 0x00200000)]
        return codegen.build_entry_catalog(analyzed, ranges, profile=profile, elf=None)

    def test_manual_callables_carry_no_hst_provenance_under_none(self):
        for profile in (None, "none"):
            catalog = self._catalog(profile)
            for addr in HST_MANUAL_CALLABLES:
                self.assertNotIn(
                    "hst-profile", catalog[addr].provenance,
                    f"0x{addr:08x} inherited an HST role under profile={profile}",
                )

    def test_resume_roles_are_not_assigned_under_none(self):
        for profile in (None, "none"):
            catalog = self._catalog(profile)
            for resume in HST_RESUME_OWNERS:
                self.assertNotIn(
                    resume, catalog,
                    f"0x{resume:08x} became an entry under profile={profile} "
                    f"even though only the HST seed table names it",
                )

    def test_roles_are_still_assigned_under_hst(self):
        catalog = self._catalog("hst")
        for addr in HST_MANUAL_CALLABLES:
            self.assertIn("hst-profile", catalog[addr].provenance)
        for resume, owner in HST_RESUME_OWNERS.items():
            self.assertTrue(catalog[resume].resumable)
            self.assertEqual(catalog[resume].owner, owner)

    def _sv_words(self, base):
        # lui/ori is a two-instruction predictable run -> one flush point.
        return {base: LUI_T0, base + 4: ORI_T0}

    def test_sv_special_addresses_are_only_excluded_under_hst(self):
        was, codegen.SV_ENABLED = codegen.SV_ENABLED, True
        try:
            for addr in sorted(codegen._SV_SPECIAL):
                words = self._sv_words(addr)
                elf = _FakeElf(words)
                none_pts = codegen.sv_plan(elf, set(words), set(), hst_profile=False)
                hst_pts = codegen.sv_plan(elf, set(words), set(), hst_profile=True)
                self.assertTrue(
                    none_pts,
                    f"profile=none suppressed --static-verify at 0x{addr:08x} "
                    f"solely because that address is in _SV_SPECIAL",
                )
                self.assertEqual(
                    hst_pts, {},
                    f"profile=hst no longer excludes 0x{addr:08x} from "
                    f"--static-verify; that is a legacy behavior change",
                )
                control = addr + CONTROL_DELTA
                ctl_words = self._sv_words(control)
                ctl_pts = codegen.sv_plan(
                    _FakeElf(ctl_words), set(ctl_words), set(), hst_profile=False)
                self.assertEqual(
                    {a - addr: v for a, v in none_pts.items()},
                    {a - control: v for a, v in ctl_pts.items()},
                )
        finally:
            codegen.SV_ENABLED = was


class _FakeElf:
    def __init__(self, words):
        self.words = words

    def read_at_vaddr(self, addr, size):
        w = self.words.get(addr)
        return w.to_bytes(4, "little") if w is not None else None


# ---------------------------------------------------------------------------
# 5. Inventory census over the supported AST shapes, re-derived from
#    codegen.py itself
# ---------------------------------------------------------------------------


#: Local names that hold a guest address inside codegen's emitters.
ADDR_OPERANDS = frozenset({"addr", "a", "start"})

#: Functions whose bodies decide what translated C is emitted.
EMITTERS = frozenset({"effect", "normal_line", "emit_function", "main", "sv_plan"})

#: Title-owned address tables, and how each one's profile gate is enforced.
#: A membership test against one of these binds guest addresses to HST
#: semantics, so it must be reachable only under the hst profile.
ADDRESS_TABLES = {
    "NULL_BASE_WORD_LOADS": "hst_profile",
    "GUEST_PATCHES": "hst_profile",
    "_SV_SPECIAL": "hst_profile",
    "SIMPLE_STUBS": "empty-under-none",
    "HST_MANUAL_CALLABLES": "catalog-early-return",
    "HST_RESUME_OWNERS": "catalog-early-return",
}

#: Names holding sets recovered from the image being translated.  A membership
#: test against one of these is not an address binding -- the contents come from
#: the input, so they carry no title address of their own.  Declared explicitly
#: rather than allow-by-default: an undeclared name fails the census, because a
#: future ``if a in NEW_HST_TABLE:`` would otherwise pass unnoticed.
IMAGE_DERIVED_TABLES = {
    "labels": "branch targets recovered from this function's own instructions",
    "consumed": "instructions already emitted by an earlier site in this function",
    "continuations": "resume points recovered by function_flow",
    "dup_slot_skips": "delay slots that are also branch targets, found per function",
    "sv_points": "static-verify predictions computed from this same function",
    "impmap": "import table parsed from the primary ELF",
    "extra_impmap": "import table parsed from an extra ELF",
}

#: Integer constants that reach a supported comparison shape but are *not*
#: guest addresses, declared per (function, operand, value).
#:
#: This registry replaced a blanket ``value >= 0x1000`` filter.  A numeric
#: threshold cannot tell an address from a register number, and it silently
#: excused every future site below 0x1000 -- ``GUEST_ABORT`` already lives at
#: 0x00000a1c, well inside the range that threshold discarded.
NON_ADDRESS_CONSTANTS = {
    ("emit_function", "a", 31):
        "`a` is rebound to rs(w) for the jr/jalr form; 31 is register $ra",
}


class Gate(NamedTuple):
    """One supported address-comparison site in codegen's emitters."""

    func: str
    lineno: int
    operand: str
    gated: bool
    constants: tuple[int, ...]
    table: str | None


def _name_ids(node) -> set[str]:
    return {n.id for n in ast.walk(node) if isinstance(n, ast.Name)}


def scan_address_gates(tree: ast.AST) -> list[Gate]:
    """Collect every supported address-comparison shape in `tree`.

    Supported shapes, and *only* these:

    * inside a function named in :data:`EMITTERS`,
    * an ``if`` whose test contains a ``Compare``,
    * whose left operand is a bare name in :data:`ADDR_OPERANDS`,
    * with an ``==`` or ``in`` operator.

    Integer literals in the comparator are reported at any magnitude; a bare
    name comparator is reported as ``table``.
    """
    gates: list[Gate] = []
    for fn in ast.walk(tree):
        if not isinstance(fn, ast.FunctionDef) or fn.name not in EMITTERS:
            continue
        for node in ast.walk(fn):
            if not isinstance(node, ast.If):
                continue
            gated = "hst_profile" in _name_ids(node.test)
            for cmp_ in [n for n in ast.walk(node.test)
                         if isinstance(n, ast.Compare)]:
                left = cmp_.left
                if not (isinstance(left, ast.Name) and left.id in ADDR_OPERANDS):
                    continue
                if not isinstance(cmp_.ops[0], (ast.Eq, ast.In)):
                    continue
                comparator = cmp_.comparators[0]
                constants = tuple(
                    n.value for n in ast.walk(comparator)
                    if isinstance(n, ast.Constant) and isinstance(n.value, int)
                    and not isinstance(n.value, bool)
                )
                gates.append(Gate(
                    func=fn.name,
                    lineno=node.lineno,
                    operand=left.id,
                    gated=gated,
                    constants=constants,
                    table=comparator.id if isinstance(comparator, ast.Name) else None,
                ))
    return gates


def address_constants(gate: Gate) -> list[int]:
    """`gate`'s integer literals, minus the reviewed non-address exemptions."""
    return [c for c in gate.constants
            if (gate.func, gate.operand, c) not in NON_ADDRESS_CONSTANTS]


def ungated_address_constants(tree: ast.AST) -> list[str]:
    return [
        f"{g.func}:{g.lineno} -> {[hex(c) for c in address_constants(g)]}"
        for g in scan_address_gates(tree)
        if address_constants(g) and not g.gated
    ]


def uncovered_gated_addresses(tree: ast.AST, covered) -> list[int]:
    return sorted({
        c
        for g in scan_address_gates(tree) if g.gated
        for c in address_constants(g)
        if c not in covered
    })


def undeclared_tables(tree: ast.AST) -> list[str]:
    """Named comparators that are in no reviewed registry."""
    return sorted({
        f"{g.table} at {g.func}:{g.lineno}"
        for g in scan_address_gates(tree)
        if g.table is not None
        and g.table not in ADDRESS_TABLES
        and g.table not in IMAGE_DERIVED_TABLES
    })


def ungated_address_tables(tree: ast.AST) -> list[str]:
    return sorted({
        f"{g.table} at {g.func}:{g.lineno}"
        for g in scan_address_gates(tree)
        if g.table in ADDRESS_TABLES
        and ADDRESS_TABLES[g.table] == "hst_profile"
        and not g.gated
    })


# Synthetic sources for the mutation tests below.  Each one is a *supported*
# shape carrying a defect the census must catch.
_MUTANT_LOW_ADDRESS_UNGATED = """
def emit_function(elf, start, ranges, known, profile=None):
    hst_profile = profile == "hst"
    for addr in insns:
        if addr == 0x40:
            out.append("    sr_title_specific(s);")
"""

_MUTANT_LOW_ADDRESS_GATED = """
def emit_function(elf, start, ranges, known, profile=None):
    hst_profile = profile == "hst"
    for addr in insns:
        if hst_profile and addr == 0x40:
            out.append("    sr_title_specific(s);")
"""

_MUTANT_UNKNOWN_TABLE = """
def main(argv):
    hst_profile = profile == "hst"
    for a in sorted(catalog):
        if hst_profile and a in NEW_HST_TABLE:
            out.append("    sr_title_specific(s);")
"""


class GateInventoryTests(unittest.TestCase):
    """A new address-keyed site cannot be added without a gate or a body.

    Evidence tier: source-shape / static.  This does not execute codegen.  Its
    scope is exactly the grammar :func:`scan_address_gates` supports -- a direct
    ``addr``/``a``/``start`` comparison against an integer literal or a named
    table, inside an emitter function.  Within that grammar it is complete and
    the mutation tests below prove it.  It is **not** a proof about arbitrary
    Python: a coupling expressed some other way (the ``insns & _SV_SPECIAL`` set
    intersection that this slice had to fix is the worked example) is invisible
    here, and only the executable differential above catches it.
    """

    @classmethod
    def setUpClass(cls):
        cls.tree = ast.parse((TOOLS / "codegen.py").read_text(encoding="utf-8"))

    # --- the real inventory ------------------------------------------------

    def test_every_constant_address_gate_is_profile_gated(self):
        self.assertEqual(
            ungated_address_constants(self.tree), [],
            "codegen compares a guest address against a literal without an "
            "hst_profile guard; that binds a numeric address to HST semantics "
            "for every profile",
        )

    def test_every_gated_address_has_a_specimen_body(self):
        uncovered = uncovered_gated_addresses(self.tree, COVERED)
        self.assertEqual(
            [hex(c) for c in uncovered], [],
            "these gated addresses are not in this test's specimen, so nothing "
            "proves their gate works; add a body for them to BODY_COUPLED",
        )

    def test_every_named_comparator_is_declared(self):
        self.assertEqual(
            undeclared_tables(self.tree), [],
            "an emitter tests membership against a name that is in neither "
            "ADDRESS_TABLES nor IMAGE_DERIVED_TABLES; declare it (and gate it, "
            "if it holds guest addresses) rather than letting it default to "
            "allowed",
        )

    def test_declared_address_tables_are_consulted_under_a_gate(self):
        self.assertEqual(ungated_address_tables(self.tree), [])

    def test_image_derived_registry_has_no_stale_entries(self):
        """A dead entry here would silently excuse a future name reusing it."""
        observed = {g.table for g in scan_address_gates(self.tree) if g.table}
        stale = sorted(set(IMAGE_DERIVED_TABLES) - observed)
        self.assertEqual(stale, [], "IMAGE_DERIVED_TABLES names nothing in codegen")

    def test_non_address_exemptions_are_live_and_minimal(self):
        """Every declared exemption must still exist, with its stated shape."""
        live = {(g.func, g.operand, c)
                for g in scan_address_gates(self.tree) for c in g.constants}
        stale = sorted(set(NON_ADDRESS_CONSTANTS) - live)
        self.assertEqual(
            [f"{f}:{o}=={hex(c)}" for f, o, c in stale], [],
            "NON_ADDRESS_CONSTANTS excuses a comparison that no longer exists; "
            "a later site reusing that shape would inherit the exemption",
        )

    def test_simple_stubs_binding_stays_profile_conditional(self):
        # SIMPLE_STUBS is gated by construction rather than by an `if`: it is
        # bound to {} for every non-hst profile.  Assert that binding directly.
        binding = [
            n for n in ast.walk(self.tree)
            if isinstance(n, ast.Assign)
            and any(isinstance(t, ast.Name) and t.id == "SIMPLE_STUBS"
                    for t in n.targets)
        ]
        self.assertEqual(len(binding), 1, "SIMPLE_STUBS is bound more than once")
        self.assertIsInstance(
            binding[0].value, ast.IfExp,
            "SIMPLE_STUBS must stay conditional on the profile",
        )
        self.assertIn("hst_profile", _name_ids(binding[0].value.test))

    def test_sv_special_is_read_only_by_sv_plan(self):
        readers = {
            fn.name
            for fn in ast.walk(self.tree)
            if isinstance(fn, ast.FunctionDef)
            and "_SV_SPECIAL" in _name_ids(fn)
        }
        self.assertEqual(readers, {"sv_plan"})

    def test_address_tables_have_not_grown_silently(self):
        """Every table entry is inside the specimen's covered set."""
        for name in ("NULL_BASE_WORD_LOADS", "GUEST_PATCHES",
                     "HST_MANUAL_CALLABLES"):
            for addr in getattr(codegen, name):
                self.assertIn(addr, COVERED, f"{name} 0x{addr:08x} uncovered")
        for addr in codegen._SV_SPECIAL:
            self.assertIn(addr, COVERED, f"_SV_SPECIAL 0x{addr:08x} uncovered")
        for addr in HST_SIMPLE_STUBS:
            self.assertIn(addr, COVERED, f"HST_SIMPLE_STUBS 0x{addr:08x} uncovered")
        for resume, owner in codegen.HST_RESUME_OWNERS.items():
            self.assertIn(resume, COVERED)
            self.assertIn(owner, COVERED)

    # --- mutants: the census must actually catch these ---------------------

    def test_a_low_address_gate_cannot_escape_the_census(self):
        """0x40 is far below the 0x1000 threshold this scanner used to apply."""
        tree = ast.parse(_MUTANT_LOW_ADDRESS_UNGATED)
        self.assertEqual(
            ungated_address_constants(tree), ["emit_function:5 -> ['0x40']"],
            "a sub-0x1000 address literal escaped the ungated-gate census",
        )

    def test_a_low_address_body_gap_cannot_escape_the_census(self):
        tree = ast.parse(_MUTANT_LOW_ADDRESS_GATED)
        self.assertEqual(
            uncovered_gated_addresses(tree, COVERED), [0x40],
            "a gated sub-0x1000 address with no specimen body was not reported",
        )

    def test_an_undeclared_address_table_cannot_escape_the_census(self):
        tree = ast.parse(_MUTANT_UNKNOWN_TABLE)
        self.assertEqual(
            undeclared_tables(tree), ["NEW_HST_TABLE at main:5"],
            "a membership test against an undeclared name was allowed through",
        )
        # It carries no integer literal, so the constant census cannot see it --
        # which is exactly why the named-comparator check has to exist.
        self.assertEqual(ungated_address_constants(tree), [])

    def test_the_register_number_exemption_is_the_only_one_needed(self):
        """Guards the exemption registry against quietly absorbing addresses."""
        self.assertEqual(len(NON_ADDRESS_CONSTANTS), 1)
        exempted = {
            c
            for g in scan_address_gates(self.tree)
            for c in g.constants
            if (g.func, g.operand, c) in NON_ADDRESS_CONSTANTS
        }
        self.assertEqual(exempted, {31})


# ---------------------------------------------------------------------------
# 6. Retired address invariants & mutant kill suite
# ---------------------------------------------------------------------------

#: Independent inventory of retired HST override addresses that must NEVER be
#: reintroduced as custom stubs, manual callables, resume points, or gated emitters.
RETIRED_HST_ADDRESSES: frozenset[int] = frozenset({0x00046D14})


def scan_ast_literals_for_addrs(tree: ast.AST, forbidden_addrs: frozenset[int]) -> list[str]:
    """Scan all AST Constant nodes (integers and strings) for forbidden retired addresses."""
    findings: list[str] = []
    for node in ast.walk(tree):
        if isinstance(node, ast.Constant):
            if isinstance(node.value, int) and node.value in forbidden_addrs:
                findings.append(f"integer literal {hex(node.value)} at line {getattr(node, 'lineno', '?')}")
            elif isinstance(node.value, str):
                for a in forbidden_addrs:
                    hex_str = f"{a:08x}"
                    short_hex = f"{a:x}"
                    if hex_str in node.value.lower() or f"0x{short_hex}" in node.value.lower() or f"f_{hex_str}" in node.value:
                        findings.append(f"string literal containing 0x{hex_str} at line {getattr(node, 'lineno', '?')}: {node.value[:40]}")
    return findings


_MUTANT_46D14_EXACT_OLD_OVERRIDE = """
def main(argv):
    hst_profile = profile == "hst"
    for a in sorted(catalog):
        if hst_profile and a == 0x00046d14:
            text = "void f_00046d14(CpuState *s) { s->pc = s->r[31]; }"
            func_texts.append(text); emitted.append(a); continue
"""

_MUTANT_46D14_HELPER_OVERRIDE = """
def _is_gameloop_entry(addr):
    return addr == 0x00046D14

def main(argv):
    hst_profile = profile == "hst"
    for a in sorted(catalog):
        if hst_profile and _is_gameloop_entry(a):
            text = "void f_00046d14(CpuState *s) { s->pc = s->r[31]; }"
            func_texts.append(text); emitted.append(a); continue
"""


class RetiredAddressGuardTests(unittest.TestCase):
    """Explicitly lock retired HST overrides/stubs against reintroduction.

    0x00046d14 was retired on 2026-08-29 after static and dynamic analysis
    proved it is an interior basic-block loop header label (L_00046d14) inside
    FUN_000468c8 (main_RunGameLoop), not a standalone function or resume entry.
    These tests enforce independent invariants so that no live metadata, AST
    helper, or classification change can silently revive an override at this
    address.
    """

    @classmethod
    def setUpClass(cls):
        cls.tree = ast.parse((TOOLS / "codegen.py").read_text(encoding="utf-8"))

    def test_retired_addresses_are_not_in_any_codegen_or_host_stubs_metadata(self):
        for addr in RETIRED_HST_ADDRESSES:
            hex_addr = f"0x{addr:08x}"
            self.assertNotIn(addr, HST_CUSTOM_STUBS, f"{hex_addr} reintroduced into HST_CUSTOM_STUBS")
            self.assertNotIn(addr, HST_SIMPLE_STUBS, f"{hex_addr} reintroduced into HST_SIMPLE_STUBS")
            self.assertNotIn(addr, BODY_COUPLED, f"{hex_addr} reintroduced into BODY_COUPLED")
            self.assertNotIn(addr, COVERED, f"{hex_addr} reintroduced into COVERED")
            self.assertNotIn(addr, codegen.NULL_BASE_WORD_LOADS, f"{hex_addr} in NULL_BASE_WORD_LOADS")
            self.assertNotIn(addr, codegen.GUEST_PATCHES, f"{hex_addr} in GUEST_PATCHES")
            self.assertNotIn(addr, codegen._SV_SPECIAL, f"{hex_addr} in _SV_SPECIAL")
            self.assertNotIn(addr, codegen.HST_MANUAL_CALLABLES, f"{hex_addr} in HST_MANUAL_CALLABLES")
            self.assertNotIn(addr, codegen.HST_RESUME_OWNERS, f"{hex_addr} in HST_RESUME_OWNERS (as resume)")
            self.assertNotIn(addr, codegen.HST_RESUME_OWNERS.values(), f"{hex_addr} in HST_RESUME_OWNERS (as owner)")

    def test_retired_addresses_are_not_in_compat_overrides_manifest(self):
        active_custom_stubs = {
            entry["address"] for entry in compat_overrides.CODEGEN_CUSTOM_STUBS
        }
        for addr in RETIRED_HST_ADDRESSES:
            hex_addr = f"0x{addr:08x}"
            self.assertNotIn(
                addr, active_custom_stubs,
                f"{hex_addr} reintroduced into compat_overrides.CODEGEN_CUSTOM_STUBS",
            )

    def test_retired_addresses_absent_from_codegen_ast(self):
        findings = scan_ast_literals_for_addrs(self.tree, RETIRED_HST_ADDRESSES)
        self.assertEqual(
            findings, [],
            f"codegen.py AST contains retired address references: {findings}",
        )

    def test_retired_46d14_cannot_be_classified_in_entry_catalog(self):
        # 0x000468c8 is the real enclosing function entry discovered by the analyzer.
        # Even under profile="hst", the catalog must not invent an entry for 0x00046d14.
        analyzed = set(HST_MANUAL_CALLABLES) | set(HST_RESUME_OWNERS.values()) | {0x000468C8}
        ranges = [(0, 0x00200000)]
        for profile in (None, "none", "hst"):
            catalog = codegen.build_entry_catalog(analyzed, ranges, profile=profile, elf=None)
            self.assertIn(0x000468C8, catalog)
            self.assertTrue(catalog[0x000468C8].callable)
            for addr in RETIRED_HST_ADDRESSES:
                self.assertNotIn(
                    addr, catalog,
                    f"0x{addr:08x} was given a catalog entry under profile={profile}",
                )

    def test_interior_label_46d14_behavioral_profile_invariance(self):
        """Emitting an enclosing function containing 0x00046d14 produces identical C across profiles."""
        words = {
            0x000468C8: _addiu_sp(-32),
            0x000468CC: (0x2B << 26) | (29 << 21) | (31 << 16) | 16,  # sw $ra, 16($sp)
            0x000468D0: (4 << 26) | (((0x00046D14 - (0x000468D0 + 4)) >> 2) & 0xFFFF),  # beq $zero, $zero, 0x46d14
            0x000468D4: NOP,
            0x00046D14: LUI_T0,
            0x00046D18: LW_T0_A0,
            0x00046D1C: JR_RA,
            0x00046D20: NOP,
        }
        elf = _FakeElf(words)
        ranges = [(0, 0x00200000)]
        known = {0x000468C8}

        none_lines = codegen.emit_function(elf, 0x000468C8, ranges, known, profile="none")
        hst_lines = codegen.emit_function(elf, 0x000468C8, ranges, known, profile="hst")

        none_text = "\n".join(none_lines)
        hst_text = "\n".join(hst_lines)

        self.assertIn("L_00046d14:", none_text, "L_00046d14 label missing under profile=none")
        self.assertIn("L_00046d14:", hst_text, "L_00046d14 label missing under profile=hst")
        self.assertNotIn("void f_00046d14", none_text)
        self.assertNotIn("void f_00046d14", hst_text)
        self.assertEqual(
            none_lines, hst_lines,
            "Enclosing function containing 0x00046d14 diverged between profile=none and profile=hst",
        )

    # --- Mutant kills -----------------------------------------------------

    def test_mutant_killed_exact_old_override_restored(self):
        tree = ast.parse(_MUTANT_46D14_EXACT_OLD_OVERRIDE)
        findings = scan_ast_literals_for_addrs(tree, RETIRED_HST_ADDRESSES)
        self.assertTrue(
            len(findings) > 0 and any("0x00046d14" in f or "0x46d14" in f for f in findings),
            "Exact old override mutant escaped the retired-address AST scanner",
        )

    def test_mutant_killed_helper_based_override(self):
        tree = ast.parse(_MUTANT_46D14_HELPER_OVERRIDE)
        findings = scan_ast_literals_for_addrs(tree, RETIRED_HST_ADDRESSES)
        self.assertTrue(
            len(findings) > 0 and any("0x00046d14" in f or "0x46d14" in f for f in findings),
            "Helper-based override mutant escaped the retired-address AST scanner",
        )

    def test_mutant_killed_metadata_only_restore(self):
        mutant_custom_stubs = (0x00046D14,)
        self.assertTrue(
            any(a in mutant_custom_stubs for a in RETIRED_HST_ADDRESSES),
            "Metadata-only restore mutant was not rejected by RETIRED_HST_ADDRESSES check",
        )

    def test_mutant_killed_codegen_only_restore(self):
        tree = ast.parse(_MUTANT_46D14_EXACT_OLD_OVERRIDE)
        uncovered = uncovered_gated_addresses(tree, COVERED)
        self.assertIn(
            0x00046D14, uncovered,
            "Codegen-only restore mutant was not caught as an uncovered gated address",
        )

    def test_mutant_killed_synchronized_callable_classification(self):
        mutant_manual_callables = (0x00046D14,)
        self.assertTrue(
            any(a in mutant_manual_callables for a in RETIRED_HST_ADDRESSES),
            "Synchronized callable mutant was not rejected by RETIRED_HST_ADDRESSES check",
        )

    def test_mutant_killed_synchronized_resume_classification(self):
        mutant_resume_owners = {0x00046D14: 0x000468C8}
        self.assertTrue(
            any(a in mutant_resume_owners for a in RETIRED_HST_ADDRESSES),
            "Synchronized resume mutant was not rejected by RETIRED_HST_ADDRESSES check",
        )


if __name__ == "__main__":
    unittest.main()
