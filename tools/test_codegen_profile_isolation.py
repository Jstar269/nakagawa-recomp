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
* the address literals themselves, which
  :meth:`GateInventoryTests.test_every_constant_address_gate_is_profile_gated`
  re-derives from ``tools/codegen.py``'s AST so a newly added site cannot be
  introduced without either a gate or a specimen body.

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

ROOT = Path(__file__).resolve().parent.parent
TOOLS = ROOT / "tools"
sys.path.insert(0, str(TOOLS))

import codegen  # noqa: E402
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
    0x00013524, 0x00015EA0, 0x000143B0, 0x00046D14, 0x0001034C, 0x000468C8,
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
# 5. Inventory completeness, re-derived from codegen.py itself
# ---------------------------------------------------------------------------


class GateInventoryTests(unittest.TestCase):
    """A new address-keyed site cannot be added without a gate or a body.

    Evidence tier: source-shape / static.  This does not execute codegen; it
    exists so the executable differential above cannot go quietly out of date.
    """

    # Comparisons whose left-hand side is a guest address in codegen's emitters.
    ADDR_OPERANDS = {"addr", "a", "start"}
    # Address tables that gate emission.  Value = how the gate is enforced.
    ADDRESS_TABLES = {
        "NULL_BASE_WORD_LOADS": "hst_profile",
        "GUEST_PATCHES": "hst_profile",
        "SIMPLE_STUBS": "empty-under-none",
        "HST_MANUAL_CALLABLES": "catalog-early-return",
        "HST_RESUME_OWNERS": "catalog-early-return",
        "_SV_SPECIAL": "hst_profile",
    }
    EMITTERS = {"effect", "normal_line", "emit_function", "main", "sv_plan"}

    @classmethod
    def setUpClass(cls):
        cls.source = (TOOLS / "codegen.py").read_text(encoding="utf-8")
        cls.tree = ast.parse(cls.source)

    @staticmethod
    def _names(node):
        return {n.id for n in ast.walk(node) if isinstance(n, ast.Name)}

    def _address_gates(self):
        """Yield (func, lineno, gated, constants) for every address comparison."""
        for fn in ast.walk(self.tree):
            if not isinstance(fn, ast.FunctionDef) or fn.name not in self.EMITTERS:
                continue
            for node in ast.walk(fn):
                if not isinstance(node, ast.If):
                    continue
                gated = "hst_profile" in self._names(node.test)
                for cmp_ in [n for n in ast.walk(node.test)
                             if isinstance(n, ast.Compare)]:
                    left = cmp_.left
                    if not (isinstance(left, ast.Name)
                            and left.id in self.ADDR_OPERANDS):
                        continue
                    if not isinstance(cmp_.ops[0], (ast.Eq, ast.In)):
                        continue
                    consts = [
                        n.value for n in ast.walk(cmp_.comparators[0])
                        if isinstance(n, ast.Constant) and isinstance(n.value, int)
                        and n.value >= 0x1000
                    ]
                    yield fn.name, node.lineno, gated, consts, cmp_.comparators[0]

    def test_every_constant_address_gate_is_profile_gated(self):
        ungated = [
            f"{fn}:{line} -> {[hex(c) for c in consts]}"
            for fn, line, gated, consts, _ in self._address_gates()
            if consts and not gated
        ]
        self.assertEqual(
            ungated, [],
            "codegen compares a guest address against a literal without an "
            "hst_profile guard; that binds a numeric address to HST semantics "
            "for every profile",
        )

    def test_every_gated_address_has_a_specimen_body(self):
        uncovered = sorted({
            c
            for _, _, gated, consts, _ in self._address_gates()
            if gated
            for c in consts
            if c not in COVERED
        })
        self.assertEqual(
            [hex(c) for c in uncovered], [],
            "these gated addresses are not in this test's specimen, so nothing "
            "proves their gate works; add a body for them to BODY_COUPLED",
        )

    def test_every_address_table_gate_is_accounted_for(self):
        """No address table may be consulted from an unreviewed place."""
        unknown = []
        for fn, line, gated, _consts, comparator in self._address_gates():
            if not isinstance(comparator, ast.Name):
                continue
            table = comparator.id
            if table not in self.ADDRESS_TABLES:
                continue
            how = self.ADDRESS_TABLES[table]
            if how == "hst_profile" and not gated:
                unknown.append(f"{table} at {fn}:{line} is consulted ungated")
        self.assertEqual(unknown, [])

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
        self.assertIn("hst_profile", self._names(binding[0].value.test))

        # _SV_SPECIAL must only ever be consulted through sv_plan's guarded arm.
        readers = {
            fn.name
            for fn in ast.walk(self.tree)
            if isinstance(fn, ast.FunctionDef)
            and "_SV_SPECIAL" in self._names(fn)
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


if __name__ == "__main__":
    unittest.main()
