# SPDX-License-Identifier: GPL-2.0-or-later

"""CI gate: every custom codegen stub and dispatch hook must be in the manifest.

2026-07-17. "Make false progress impossible": a game-address-specific override
that changes what the recompiled game does must be visible and classified, not
just an unremarked `if a == 0x...:` someone added during a debugging session.
This test extracts the authoritative, structured sources of such overrides
--

  1. tools/codegen.py: GUEST_PATCHES, host_stubs.HST_SIMPLE_STUBS, and the
     per-address custom stubs between the "--- CUSTOM STUBS START/END ---"
     markers.
  2. src/rt/recomp.c: the g_exact_hooks[]/g_range_hooks[] DispatchHook tables.
  3. src/rt/hle.c: guest addresses used as addresses through the MEM_*
     accessors, sr_r32/sr_w32, dispatch, or the ge_call_guest* nested-guest
     call helpers.  Added 2026-08-20 (title-2 readiness): before that, 38
     distinct title addresses across 50 sites in hle.c -- including a whole
     guest display-driver bring-up dispatched from sceDisplaySetMode and a
     read-only umd.ufl head dump reached through a cast-wrapped
     MEM_R8((uint32_t)(...)) shape -- sat outside this inventory entirely.

-- and fails if any source contains an address/hook this manifest
(tools/compat_overrides.py) does not know about, or if the manifest lists an
address that no longer exists in its source (a stale entry masking that the
override was actually removed). Each real entry must also carry one of the
five documented categories and, for src/rt/hle.c groups, exactly one title-2
readiness census bucket plus the five review answers for override-classified
groups.

SCANNER CONTRACT.  The extractor recognizes two families of shape.

DIRECT shapes: a guest-address literal written inside the call itself --
MEM_R*/MEM_W*, sr_r32/sr_w32, dispatch, ge_call_guest*, and VRAM-window
``return`` literals.

INDIRECT shapes (added 2026-08-21): a guest-address literal bound to a name
first and reaching guest state through that name.  Before this, an entire
title coupling was invisible to the gate while the census reported itself
complete at 38/38: sceDisplaySetMode -> ensure_runtime_sync_callbacks reads and
writes an HST configuration block through ``const uint32_t config =
0x00333138u``, may create an HLE semaphore whose name pointer arrives as
``call.r[4] = 0x002bdf38u``, and installs six guest wrapper entry points that
are assigned to locals (``enter = 0x000823f0u``) and only then stored into guest
memory.  Eight title addresses, none of them ever written inside a MEM_* call,
so the direct regex matched none of them.  Two shapes now cover that family:

  bound_local          [const] uint32_t NAME = <literal>;   (or a later
                       NAME = <literal>; to a name already bound in the same
                       function) where NAME is afterwards used, in that same
                       function, in a guest-coupling position: the ADDRESS
                       argument of MEM_*/sr_r32/sr_w32/dispatch/ge_call_guest*,
                       the VALUE argument of a MEM_W* (the name is stored into
                       guest memory), or the right-hand side of a CpuState
                       register assignment.

  cpu_state_register   <expr>.r[N] = <literal>;  /  s->r[N] = <literal>;
                       -- a literal handed directly to guest code.

This is a BOUNDED GRAMMAR, not a C analyzer, and its limits are deliberate:

  * The indirect shapes require the literal to be 4-byte aligned.  A MIPS code
    address always is, and so is a word-addressed data base; the alignment rule
    is what keeps ``s->r[3] = 0xFFFFFFFF`` (an errno) and ``s->r[24] =
    0xDEADBEEFu`` (poison) out of the inventory without resorting to a
    magnitude heuristic, which this gate rejects on principle.  An unaligned
    guest byte address reached indirectly is therefore NOT covered -- reached
    directly through MEM_R8 it still is.
  * The coupling use must appear in the SAME function body as the binding.  A
    literal bound in one function and consumed in another is not covered.
  * Only literal-to-name binding is followed.  An address computed at runtime,
    assembled from parts, or read out of a table is not covered by either
    family.
  * A binding must be the whole statement on its line.  ``if (m) { enter =
    0x...; }`` is not matched; the .clang-format'd one-statement-per-line shape
    the real handler uses is.  This limit is measured, not assumed -- the test
    that first exercised the reassignment shape wrote it inline and did not
    match.
  * Struct initializers and array tables are not covered.

New absolute guest address sites are mechanically enumerable precisely because
they must appear through one of the supported shapes to be admitted; the census
test pins the current counts so a shape-set change is a deliberate, reviewed
act.  Anything outside the grammar above must be kept visible by the other
inventory mechanisms.

Deliberately out of scope: the scattered `s->pc == 0x...`/`entry == 0x...`
diagnostic trace points across src/rt/sched.c are not mechanically extracted
here (too many different call shapes for a robust regex, and they are
read-only/env-gated by construction -- see compat_overrides.DIAGNOSTIC_GROUPS
for the manually-maintained list). The scheduler-level behavior-altering hooks
in compat_overrides.SCHEDULER_HOOKS are likewise documented manually, not
cross-checked automatically, for the same reason. If those call shapes ever
settle into a small stable set, extending this gate to cover them is the
natural next step -- do not read their absence here as "safe to ignore."
"""

import inspect
import re
import unittest
from pathlib import Path

import codegen
import compat_overrides
from host_stubs import HST_SIMPLE_STUBS

REPO_ROOT = Path(__file__).resolve().parent.parent
RECOMP_C = REPO_ROOT / "src" / "rt" / "recomp.c"


def extract_codegen_custom_stub_addresses() -> set[int]:
    src = inspect.getsource(codegen)
    m = re.search(r"# --- CUSTOM STUBS START ---(.*?)# --- CUSTOM STUBS END ---", src, re.S)
    assert m, "codegen.py: CUSTOM STUBS START/END markers not found (did the driver loop move?)"
    block = m.group(1)
    addrs: set[int] = set()
    for stmt in re.finditer(r"if (?:hst_profile and )?a (?:==|in) \(?(0x[0-9a-fA-F]+(?:\s*,\s*0x[0-9a-fA-F]+)*)\)?:", block):
        group1 = stmt.group(1)
        if not group1:
            continue
        for h in re.findall(r"0x[0-9a-fA-F]+", group1):
            addrs.add(int(h, 16))
    return addrs


def extract_dispatch_hook_table(table_name: str) -> list[tuple[int, str]]:
    src = RECOMP_C.read_text(encoding="utf-8")
    m = re.search(r"static const DispatchHook " + re.escape(table_name) + r"\[\] = \{(.*?)\n\};", src, re.S)
    assert m, f"src/rt/recomp.c: {table_name}[] not found (did it get renamed/restructured?)"
    rows = re.findall(r'\{\s*(0x[0-9a-fA-F]+|0)u?,\s*(0x[0-9a-fA-F]+|0)u?,\s*"([^"]+)"', m.group(1))
    return [(int(addr, 16), name) for addr, _mask, name in rows]


HLE_C = REPO_ROOT / "src" / "rt" / "hle.c"


# src/rt/hle.c does not have a single structured table the way codegen.py and
# recomp.c do, but the ways a guest address is actually USED as an address there
# are a small, stable set: the MEM_* accessors, sr_r32/sr_w32, dispatch, and the
# nested-guest-call helpers.  Any absolute guest address reached through one of
# those is a guest location this runtime knows about by number -- regardless of
# magnitude.  Cast-wrapped address bases (e.g. MEM_R8((uint32_t)(0x... + off)))
# are part of the shape set; a literal in a VALUE position (second argument of
# MEM_W32, an ioctl/error code, a size) is not an address usage and is not
# scanned.
#
# One additional shape is scanned: a return statement carrying a literal in the
# architectural VRAM window (0x04000000..0x041fffff).  A PSP constant handed
# back to the guest as a value (e.g. the EDRAM base from sceGeEdramGetAddr) is
# generic PSP semantics, but only when an explicit site rule in
# compat_overrides.HLE_GENERIC_SITE_RULES names that exact function+shape+literal
# site.  Error codes and other high literals are excluded from this shape by
# the window check, so the scan stays narrow.
#
# Magnitude is not a semantic classifier, and neither is a hardware window.
# There is deliberately no address ceiling and no whole-region VRAM exemption:
# a title-specific operation such as MEM_W32(0x04012340u, 1) or
# MEM_W32(0x08901234u, 1) must not escape detection because the address is
# numerically high or points into VRAM.  Generic PSP constants are exempted
# only through the narrow, explicit site rules in
# compat_overrides.HLE_GENERIC_SITE_RULES (exact function + shape + literal),
# which this module also verifies mechanically.
HLE_GUEST_ADDRESS_RE = re.compile(
    r"(?:"
    r"(?:MEM_(?:R|W)(?:8|16|32)|sr_r32|sr_w32|dispatch)"
    r"\s*\(\s*(?:s\s*,\s*)?(?:\(uint32_t\)\s*\(\s*)?(0[xX][0-9a-fA-F]{5,8})"
    r"|"
    r"ge_call_guest(?:_rv)?\s*\(\s*s\s*,\s*(0[xX][0-9a-fA-F]{5,8})"
    r")"
)

#: Architectural VRAM window: any PSP constant returned from this window is a
#: hardware fact candidate.  The window check keeps this shape from sweeping
#: in every error-code/size return in hle.c.
VRAM_WINDOW_FIRST = 0x04000000
VRAM_WINDOW_LAST = 0x041fffff

HLE_ARCH_RETURN_RE = re.compile(r"return\s+(0[xX][0-9a-fA-F]{5,8})u?\s*;")

#: A function-body opener.  Deliberately broader than the historical
#: ``h_<name>(CpuState *s)`` pattern: the coupling that motivated the indirect
#: grammar lives in ``ensure_runtime_sync_callbacks``, a static helper whose name
#: does not start with ``h_``, so every site inside it was attributed to whichever
#: h_ handler happened to be defined above it -- or to "" for the sites that
#: precede the first one.  Widening this changed no address, line or shape in the
#: existing census; it only replaced 27 wrong or empty attributions with real
#: ones, and no generic-site exemption flips as a result.
HLE_FUNCTION_RE = re.compile(r"^static\s+[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\(")

#: Indirect grammar.  See SCANNER CONTRACT above for the limits these accept.
HLE_LITERAL = r"0[xX][0-9a-fA-F]{5,8}"
HLE_BIND_DECL_RE = re.compile(
    rf"^\s*(?:const\s+)?uint32_t\s+([A-Za-z_]\w*)\s*=\s*({HLE_LITERAL})u?\s*;")
HLE_BIND_ASSIGN_RE = re.compile(rf"^\s*([A-Za-z_]\w*)\s*=\s*({HLE_LITERAL})u?\s*;")
HLE_REGISTER_ASSIGN_RE = re.compile(
    rf"(?:\w+\s*\.|\w+\s*->|s\s*->)\s*r\s*\[\s*\d+\s*\]\s*=\s*({HLE_LITERAL})u?\s*;")


def _hle_coupling_use_patterns(name: str) -> list[re.Pattern]:
    """Positions in which NAME carries a guest address into guest-visible state."""
    n = re.escape(name)
    return [
        # ADDRESS argument of a guest memory access
        re.compile(rf"(?:MEM_[RW](?:8|16|32)|sr_r32|sr_w32)\s*\(\s*(?:\(uint32_t\)\s*\(\s*)?{n}\b"),
        # dispatch target / nested guest call target
        re.compile(rf"(?:dispatch|ge_call_guest(?:_rv)?)\s*\(\s*s\s*,\s*{n}\b"),
        # VALUE argument of a guest memory WRITE: the name is stored INTO the guest
        re.compile(rf"MEM_W(?:8|16|32)\s*\([^;]*,\s*{n}\s*\)"),
        # handed to guest code through a CpuState register
        re.compile(rf"r\s*\[\s*\d+\s*\]\s*=\s*{n}\s*;"),
    ]


def hle_function_spans(lines: list[str]) -> list[tuple[str, int, int]]:
    """(name, first_line, last_line) for each static function body, 1-based.

    Brace counting, not parsing: the file is .clang-format'd, so a body opens on
    the signature line or the one after it and closes at depth zero.
    """
    spans: list[tuple[str, int, int]] = []
    index, total = 0, len(lines)
    while index < total:
        stripped = lines[index].rstrip()
        match = HLE_FUNCTION_RE.match(stripped)
        # A forward declaration has the same shape as a definition. Treating one as an
        # opener makes the brace walk below swallow the NEXT function's body and
        # attribute its sites to the declared name -- which is exactly what happened to
        # h_DisplaySetMode, defined directly under the ensure_runtime_sync_callbacks
        # prototype.
        if not match or stripped.endswith(";"):
            index += 1
            continue
        depth, cursor, opened = 0, index, False
        while cursor < total:
            depth += lines[cursor].count("{") - lines[cursor].count("}")
            if "{" in lines[cursor]:
                opened = True
            if opened and depth <= 0:
                break
            cursor += 1
        spans.append((match.group(1), index + 1, cursor + 1))
        index = cursor + 1
    return spans


def extract_hle_indirect_sites(source: str) -> dict[int, list[tuple[int, str, str]]]:
    """Guest addresses that reach guest state through a name rather than a call.

    Returns the same address -> (line, function, shape) mapping as the direct
    scan, with shape ``bound_local`` or ``cpu_state_register``.
    """
    lines = source.splitlines()
    sites: dict[int, list[tuple[int, str, str]]] = {}
    for name, start, end in hle_function_spans(lines):
        body = lines[start - 1:end]
        bound: dict[str, list[tuple[int, int]]] = {}
        for offset, line in enumerate(body):
            binding = HLE_BIND_DECL_RE.match(line) or HLE_BIND_ASSIGN_RE.match(line)
            if binding:
                bound.setdefault(binding.group(1), []).append(
                    (start + offset, int(binding.group(2), 16)))
            register = HLE_REGISTER_ASSIGN_RE.search(line)
            if register:
                value = int(register.group(1), 16)
                if value % 4 == 0 and not compat_overrides.is_generic_site(
                        name, "cpu_state_register", value):
                    sites.setdefault(value, []).append(
                        (start + offset, name, "cpu_state_register"))
        joined = "\n".join(body)
        for variable, bindings in bound.items():
            if not any(p.search(joined) for p in _hle_coupling_use_patterns(variable)):
                continue
            for lineno, value in bindings:
                if value % 4 != 0:
                    continue
                if compat_overrides.is_generic_site(name, "bound_local", value):
                    continue
                sites.setdefault(value, []).append((lineno, name, "bound_local"))
    return sites


def extract_hle_guest_sites(source: str) -> dict[int, list[tuple[int, str, str]]]:
    """Map guest address -> (1-based line, enclosing function, shape) tuples.

    ``shape`` is ``MEM_R8``/``MEM_W32``/``dispatch``/``ge_call_guest``/
    ``ge_call_guest_rv``/``sr_r32``/``sr_w32``/``return``.  A site is dropped
    from the result only when compat_overrides.is_generic_site() matches an
    explicit rule for its exact function+shape+address.
    """
    found: dict[int, list[tuple[int, str, str]]] = {}
    current_function = ""
    for lineno, line in enumerate(source.splitlines(), 1):
        fn = HLE_FUNCTION_RE.search(line)
        # Same forward-declaration rule as hle_function_spans(): a prototype names a
        # function whose body is somewhere else, so it must not claim the sites that
        # follow it.
        if fn and not line.rstrip().endswith(";"):
            current_function = fn.group(1)
        for m in HLE_GUEST_ADDRESS_RE.finditer(line):
            literal = m.group(1) or m.group(2)
            address = int(literal, 16)
            shape = _shape_for(line)
            if compat_overrides.is_generic_site(current_function, shape, address):
                continue
            found.setdefault(address, []).append((lineno, current_function, shape))
        for m in HLE_ARCH_RETURN_RE.finditer(line):
            address = int(m.group(1), 16)
            if not (VRAM_WINDOW_FIRST <= address <= VRAM_WINDOW_LAST):
                continue
            if compat_overrides.is_generic_site(current_function, "return", address):
                continue
            found.setdefault(address, []).append((lineno, current_function, "return"))
    # Indirect shapes are merged into the same result, so every coverage, census
    # and staleness test below sees one address space rather than two.
    for address, sites in extract_hle_indirect_sites(source).items():
        found.setdefault(address, []).extend(sites)
    for sites in found.values():
        sites.sort()
    return found


def _shape_for(line: str) -> str:
    """Name the call shape on a scanned line.  The shape is the semantic
    context an exemption rule must match: a rule may exempt a ``return`` site
    (architectural constant handed to the guest) but never a memory-access or
    dispatch shape."""
    match = re.search(r"(MEM_[RW](?:8|16|32)|sr_r32|sr_w32|dispatch|ge_call_guest_rv|ge_call_guest)", line)
    return match.group(1) if match else "unknown"


def extract_hle_guest_addresses(source: str) -> dict[int, list[int]]:
    """Map guest address -> 1-based line numbers where hle.c uses it as an address."""
    return {a: [ln for ln, _fn, _shape in sites]
            for a, sites in extract_hle_guest_sites(source).items()}


def hle_inventoried_addresses() -> set[int]:
    result: set[int] = set()
    for group in compat_overrides.HLE_GUEST_ADDRESS_GROUPS:
        result.update(group["addresses"])
    return result


class HleGuestAddressCoverageTests(unittest.TestCase):
    """src/rt/hle.c is a generic PSP HLE layer by name. Every address in it that
    only means something in one title's memory map is title coupling, and has to
    be visible as such."""

    def test_extractor_is_not_vacuous(self):
        """BLIND-SPOT REGRESSION: a regex that silently stops matching (or a scan
        scope that drops hle.c) would make every coverage test below pass for the
        wrong reason, so assert it still finds the real sites. Removing hle.c
        from the scanned scopes must kill this test."""
        found = extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8"))
        self.assertGreater(len(found), 20,
                           "the hle.c guest-address extractor matched almost nothing; the call "
                           "shapes it looks for have probably changed or the scanned scope "
                           "dropped hle.c, and its coverage tests are now vacuous rather than "
                           "passing")

    def test_every_hle_guest_address_is_inventoried(self):
        """CLEAN POSITIVE / contamination gate: the current hle.c passes, and any
        uninventoried title address added to it fails here."""
        found = extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8"))
        missing = set(found) - hle_inventoried_addresses()
        self.assertFalse(
            missing,
            "src/rt/hle.c uses guest address(es) that tools/compat_overrides.py "
            "HLE_GUEST_ADDRESS_GROUPS does not account for: "
            + ", ".join(f"0x{a:08x} (line {found[a][0]})" for a in sorted(missing))
            + ". A generic PSP handler that knows a specific title's addresses is "
              "semantic debt; classify it rather than adding it silently.")

    def test_manifest_has_no_stale_hle_entries(self):
        """CLASSIFICATION DRIFT: if an inventoried site disappears or materially
        changes shape (so the extractor no longer sees it), this fails instead of
        silently preserving stale metadata."""
        found = extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8"))
        stale = hle_inventoried_addresses() - set(found)
        self.assertFalse(
            stale,
            "tools/compat_overrides.py lists src/rt/hle.c guest address(es) that no longer "
            "appear there (stale entry masking a removed override): "
            + ", ".join(f"0x{a:08x}" for a in sorted(stale)))

    def test_every_hle_group_has_a_valid_category_and_bucket(self):
        for group in compat_overrides.HLE_GUEST_ADDRESS_GROUPS:
            self.assertIn(group["category"], compat_overrides.CATEGORIES, group["name"])
            self.assertIn(group["title2_bucket"], compat_overrides.TITLE2_BUCKETS, group["name"])
            self.assertTrue(group["reason"].strip(), group["name"])
            self.assertTrue(group["source"].strip(), group["name"])
            self.assertTrue(group["title_scope"].strip(), group["name"])

    def test_override_groups_answer_the_five_review_questions(self):
        """EXPLICIT_COMPATIBILITY_OVERRIDE groups must carry enough metadata to answer:
        why does this exist / what title scopes it / what generic fallback remains /
        what evidence justifies it / can another title inherit it accidentally."""
        required = {"reason", "title_scope", "generic_fallback", "evidence",
                    "accidental_inheritance"}
        for group in compat_overrides.HLE_GUEST_ADDRESS_GROUPS:
            if group["title2_bucket"] != "EXPLICIT_COMPATIBILITY_OVERRIDE":
                continue
            missing = required - set(group)
            self.assertFalse(
                missing, f"{group['name']}: EXPLICIT_COMPATIBILITY_OVERRIDE group missing "
                         f"review fields {sorted(missing)}")

    def test_extractor_detects_a_newly_introduced_guest_address(self):
        """DELIBERATE CONTAMINATION NEGATIVE (fixture): the gate is only worth having
        if it actually fires on the shape it exists to catch, so contaminate a
        snippet deliberately."""
        contaminated = (
            "static uint32_t h_SomeGenericPspCall(CpuState *s) {\n"
            "    if (MEM_R32(0x00abcdefu) != 1u) return 0;\n"
            "    ge_call_guest(s, 0x00123456u, 0, 0, 0);\n"
            "    return 0;\n"
            "}\n"
        )
        found = extract_hle_guest_addresses(contaminated)
        self.assertEqual(sorted(found), [0x00123456, 0x00abcdef])
        self.assertEqual(found[0x00abcdef], [2])
        self.assertFalse(set(found) <= hle_inventoried_addresses(),
                         "the negative fixture's invented addresses must not already be "
                         "inventoried, or this test proves nothing")

    def test_vram_window_memory_accesses_are_flagged(self):
        """VRAM BLIND-SPOT REGRESSION (A/B): a direct fixed MEM_R/MEM_W at an
        arbitrary VRAM address must be inventoried, not silently classified
        generic because it points into the 0x04000000..0x041fffff hardware
        window.  A whole-region VRAM exemption is a blind spot by construction:
        a title-specific write such as MEM_W32(0x04012340u, v) is title
        coupling even though the address is VRAM geometry."""
        vram_sites = (
            "    MEM_W32(0x04012340u, 1);\n"
            "    uint32_t px = MEM_R32(0x04100000u);\n"
        )
        found = extract_hle_guest_addresses(vram_sites)
        self.assertEqual(sorted(found), [0x04012340, 0x04100000],
                         "VRAM-window memory accesses must be scanned; no whole-region "
                         "exemption may swallow them")
        self.assertFalse(compat_overrides.is_generic_site("h_Fake", "MEM_W32", 0x04012340))
        self.assertFalse(compat_overrides.is_generic_site("h_Fake", "MEM_R32", 0x04100000))

    def test_high_ram_addresses_are_flagged_not_ceilinged(self):
        """BLIND-SPOT REGRESSION (C): a title-specific operation at a numerically
        high absolute guest address must be flagged.  The original design skipped
        every address >= 0x04000000, so MEM_W32(0x08901234u, 1) -- a write into
        user RAM -- escaped detection entirely.  The repaired design scans every
        absolute address and excludes only narrow, explicit generic rules."""
        ram_writes = (
            "    if (MEM_W32(0x08901234u, 1)) return 0;\n"
            "    uint32_t v = MEM_R32(0x09abcdefu);\n"
            "    ge_call_guest(s, 0x0a000000u, 0, 0, 0);\n"
        )
        found = extract_hle_guest_addresses(ram_writes)
        self.assertEqual(sorted(found), [0x08901234, 0x09abcdef, 0x0a000000],
                         "high absolute guest addresses must be scanned; the blanket "
                         "0x04000000 ceiling is removed")

    def test_cast_wrapped_high_ram_read_is_detected(self):
        """CAST-WRAPPED SHAPE (D): MEM_R8((uint32_t)(0x09abcdefu + i)) -- a
        cast-wrapped absolute address base -- must be detected.  This shape was
        invisible to the original extractor and hid the umd.ufl head dump site
        (0x0030b8d0) from the census."""
        cast_wrapped = (
            "static uint32_t h_Dump(CpuState *s) {\n"
            "    for (int i = 0; i < 16; i++)\n"
            "        putchar(MEM_R8((uint32_t)(0x09abcdefu + i)));\n"
            "    return 0;\n"
            "}\n"
        )
        found = extract_hle_guest_addresses(cast_wrapped)
        self.assertIn(0x09abcdef, found,
                      "cast-wrapped absolute address bases are part of the supported "
                      "direct-literal shape set")

    def test_edram_base_return_is_generic_only_by_site_rule(self):
        """GENERIC-CONTEXT CASE (E): the architectural EDRAM base returned by
        sceGeEdramGetAddr is a hardware constant, but it is generic ONLY because
        an explicit site rule names that exact function+shape+literal site --
        not because of a whole-region exemption.  The same literal through a
        MEM access, or returned from any other function, must be flagged."""
        generic = (
            "static uint32_t h_GeEdramGetAddr(CpuState *s) { (void)s; return 0x04000000; }\n"
        )
        found = extract_hle_guest_addresses(generic)
        self.assertEqual(found, {},
                         "the EDRAM-base return site must be exempted by its site rule")
        self.assertTrue(compat_overrides.is_generic_site(
            "h_GeEdramGetAddr", "return", 0x04000000))
        self.assertFalse(compat_overrides.is_generic_site(
            "h_Other", "return", 0x04000000))
        self.assertFalse(compat_overrides.is_generic_site(
            "h_GeEdramGetAddr", "MEM_R32", 0x04000000),
            "the site rule exempts the return only; a MEM access at the same "
            "address stays inventoried")

    def test_generic_site_rules_are_narrow_and_reviewable(self):
        """GENERIC-SITE CONTRACT: the exemption mechanism itself is visible and
        mechanically tested.  Every rule must name an exact function, an exact
        shape, an exact literal, and the generic PSP fact it stands for.  Rules
        may exempt only non-memory shapes (``return``): a rule may never exempt
        a memory-access or dispatch shape, because a direct fixed MEM_R/MEM_W
        at an arbitrary VRAM address is exactly the shape a title-specific
        coupling takes."""
        self.assertTrue(compat_overrides.HLE_GENERIC_SITE_RULES,
                        "the generic-site rule list must be non-empty and visible")
        for rule in compat_overrides.HLE_GENERIC_SITE_RULES:
            for key in ("name", "function", "shape", "address", "reason"):
                self.assertIn(key, rule, f"site rule missing {key}: {rule}")
            self.assertTrue(rule["name"].strip())
            self.assertTrue(rule["function"].strip())
            self.assertTrue(rule["reason"].strip())
            self.assertEqual(rule["shape"], "return",
                             f"site rule {rule['name']} exempts a memory-access or "
                             "dispatch shape; only non-memory return sites may be "
                             "generic")
            self.assertTrue(VRAM_WINDOW_FIRST <= rule["address"] <= VRAM_WINDOW_LAST,
                            f"site rule {rule['name']} addresses {rule['address']:#x}, "
                            "outside the architectural window scanned for return sites")

    def test_site_rules_exempt_only_their_exact_site(self):
        """SITE-RULE PRECISION: each rule must exempt exactly its own site --
        no neighbor literals, no other functions, no memory shapes."""
        for rule in compat_overrides.HLE_GENERIC_SITE_RULES:
            address = rule["address"]
            self.assertTrue(compat_overrides.is_generic_site(
                rule["function"], rule["shape"], address))
            self.assertFalse(compat_overrides.is_generic_site(
                "h_Other", rule["shape"], address))
            self.assertFalse(compat_overrides.is_generic_site(
                rule["function"], "MEM_R32", address))
            self.assertFalse(compat_overrides.is_generic_site(
                rule["function"], rule["shape"], address + 1))

    def test_census_counts_are_reconciled_exactly(self):
        """CENSUS RECONCILIATION (F): the extractor must currently find exactly
        46 distinct addresses across 59 sites in src/rt/hle.c, with every site
        inventoried and no stale inventory entries.  If the corrected scanner
        legitimately changes these counts (for example after src/rt/hle.c
        changes land from #91/#92), update the census and this assertion
        together -- never silently.

        38/50 -> 46/59 on 2026-08-21 when the indirect grammar landed: the eight
        ensure_runtime_sync_callbacks addresses, plus a second (bound_local) site
        for 0x0030b8d0, which the direct scan already saw through its
        cast-wrapped MEM_R8 shape."""
        found = extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8"))
        self.assertEqual(len(found), 46,
                         "distinct-address census drifted from the reconciled 46; "
                         "recompute the census deliberately if the scanner "
                         "legitimately changed")
        self.assertEqual(sum(len(v) for v in found.values()), 59,
                         "site-count census drifted from the reconciled 59; "
                         "recompute the census deliberately if the scanner "
                         "legitimately changed")
        missing = set(found) - hle_inventoried_addresses()
        stale = hle_inventoried_addresses() - set(found)
        self.assertEqual(missing, set())
        self.assertEqual(stale, set())

    def test_old_ceiling_design_would_have_missed_high_ram_writes(self):
        """FAILING-BEFORE PROOF (A): reproduce the original extractor's ceiling
        decision on the A fixture and show it would have missed the coupling the
        repaired design flags."""
        ram_writes = "    if (MEM_W32(0x08901234u, 1)) return 0;\n"
        found = extract_hle_guest_addresses(ram_writes)
        self.assertIn(0x08901234, found,
                      "the repaired design must flag the high RAM write")
        old_ceiling_result = [a for a in found if a < 0x04000000]
        self.assertEqual(
            old_ceiling_result, [],
            "the original 'skip everything >= 0x04000000' rule would have returned "
            "nothing for this fixture -- the exact blind spot this repair removes")

    def test_unclassified_new_literal_fails_closed(self):
        """FAIL-CLOSED (E): a new literal that is neither inventoried nor covered
        by a generic rule must fail the gate, not pass silently.  This mirrors the
        mutant the suite is verified against."""
        contaminated = (
            "static uint32_t h_Unknown(CpuState *s) {\n"
            "    return MEM_R32(0x08f00000u);\n"
            "}\n"
        )
        found = extract_hle_guest_addresses(contaminated)
        missing = set(found) - hle_inventoried_addresses()
        self.assertTrue(missing,
                        "an unclassified high RAM literal must surface as missing "
                        "from the inventory (fail closed), not be swallowed")
        self.assertFalse(compat_overrides.is_generic_site("h_Unknown", "MEM_R32", 0x08f00000),
                         "the injected literal must not be a generic site, or this "
                         "test proves nothing")


class CompatManifestCoverageTests(unittest.TestCase):
    def test_every_override_has_a_valid_category(self):
        for o in compat_overrides.OVERRIDES + compat_overrides.DISPATCH_RANGE_HOOKS:
            self.assertIn(o["category"], compat_overrides.CATEGORIES,
                          f"{o.get('name', o.get('address'))}: invalid category {o.get('category')!r}")

    def test_codegen_guest_patches_are_documented(self):
        documented = compat_overrides.all_documented_addresses()
        undocumented: set[int] = set(codegen.GUEST_PATCHES.keys()) - documented
        self.assertEqual(undocumented, set(),
                          f"tools/codegen.py GUEST_PATCHES not in tools/compat_overrides.py: "
                          f"{sorted(hex(a) for a in undocumented)}")

    def test_hst_simple_stubs_are_documented(self):
        documented = compat_overrides.all_documented_addresses()
        undocumented: set[int] = set(HST_SIMPLE_STUBS.keys()) - documented
        self.assertEqual(undocumented, set(),
                          f"tools/host_stubs.py HST_SIMPLE_STUBS not in tools/compat_overrides.py: "
                          f"{sorted(hex(a) for a in undocumented)}")

    def test_hst_entry_roles_are_documented_exactly(self):
        documented = {
            int(item["address"]): (str(item["role"]), item.get("owner"))
            for item in compat_overrides.HST_ENTRY_ROLES
        }
        expected = {
            **{addr: ("callable", None) for addr in codegen.HST_MANUAL_CALLABLES},
            **{addr: ("resume", owner) for addr, owner in codegen.HST_RESUME_OWNERS.items()},
        }
        self.assertEqual(documented, expected)

    def test_codegen_custom_stubs_are_documented(self):
        found = extract_codegen_custom_stub_addresses()
        documented = compat_overrides.all_documented_addresses()
        undocumented: set[int] = found - documented
        self.assertEqual(undocumented, set(),
                          f"tools/codegen.py custom stub(s) not in tools/compat_overrides.py "
                          f"(add a CODEGEN_CUSTOM_STUBS entry): {sorted(hex(a) for a in undocumented)}")

    def test_dispatch_exact_hooks_are_documented(self):
        found = extract_dispatch_hook_table("g_exact_hooks")
        self.assertGreater(len(found), 0, "g_exact_hooks[] parsed as empty -- regex likely broken")
        documented = compat_overrides.all_documented_addresses()
        undocumented = [(addr, name) for addr, name in found if addr not in documented]
        self.assertEqual(undocumented, [],
                          f"src/rt/recomp.c g_exact_hooks[] entries not in tools/compat_overrides.py "
                          f"(add a DISPATCH_HOOKS entry): {undocumented}")

    def test_dispatch_range_hooks_are_documented_by_name(self):
        found_names: set[str] = {name for _addr, name in extract_dispatch_hook_table("g_range_hooks")}
        self.assertGreater(len(found_names), 0, "g_range_hooks[] parsed as empty -- regex likely broken")
        documented_names: set[str] = {str(o["name"]) for o in compat_overrides.DISPATCH_RANGE_HOOKS if "name" in o}
        self.assertEqual(found_names - documented_names, set(),
                          f"src/rt/recomp.c g_range_hooks[] entries not in "
                          f"tools/compat_overrides.py DISPATCH_RANGE_HOOKS: {found_names - documented_names}")

    def test_manifest_has_no_stale_codegen_entries(self):
        """An entry that claims to come from codegen.py but no longer matches any real
        stub/patch would silently stop being enforced -- catch that drift too."""
        live: set[int] = (extract_codegen_custom_stub_addresses()
                | set(codegen.GUEST_PATCHES.keys())
                | set(HST_SIMPLE_STUBS.keys()))
        claimed: set[int] = {int(o["address"]) for o in
                   compat_overrides.GUEST_PATCHES + compat_overrides.CODEGEN_CUSTOM_STUBS
                   + compat_overrides.HST_SIMPLE_STUBS if "address" in o}
        stale = claimed - live
        self.assertEqual(stale, set(),
                          f"tools/compat_overrides.py lists codegen.py address(es) that no longer "
                          f"exist in tools/codegen.py/host_stubs.py (stale entry): "
                          f"{sorted(hex(a) for a in stale)}")

    def test_manifest_has_no_stale_dispatch_hook_entries(self):
        live: set[int] = {addr for addr, _name in extract_dispatch_hook_table("g_exact_hooks")}
        claimed: set[int] = {int(o["address"]) for o in compat_overrides.DISPATCH_HOOKS if "address" in o}
        stale = claimed - live
        self.assertEqual(stale, set(),
                          f"tools/compat_overrides.py DISPATCH_HOOKS lists address(es) no longer in "
                          f"src/rt/recomp.c g_exact_hooks[] (stale entry): {sorted(hex(a) for a in stale)}")


#: The eight ensure_runtime_sync_callbacks addresses, re-derived from src/rt/hle.c
#: rather than copied from a report. They are pinned here so a silent edit to the
#: handler shows up as a test failure and not as a quietly shrinking census.
RUNTIME_SYNC_CALLBACK_SITES = {
    0x00333138: "configuration block base",
    0x002BDF38: "semaphore name pointer handed to the guest in $a0",
    0x000823F0: "mode 0 enter (CpuSuspendIntr wrapper)",
    0x00082438: "mode 0 leave (CpuResumeIntr wrapper)",
    0x00082474: "mode 1 enter (semaphore wait wrapper)",
    0x0008249C: "mode 1 leave (semaphore signal wrapper)",
    0x000824C0: "mode 2 enter (lightweight mutex lock wrapper)",
    0x000824E8: "mode 2 leave (lightweight mutex unlock wrapper)",
}


class HleIndirectCouplingGrammar(unittest.TestCase):
    """The indirect grammar: a guest address bound to a NAME, not written inside
    the call that uses it.

    This class exists because the census was reporting itself complete at 38/38
    while an entire title coupling -- a configuration block, a semaphore name
    pointer and six guest wrapper entry points, all reached from an
    unconditionally registered sceDisplaySetMode -- was invisible to the gate.
    Every literal there is bound to a local or assigned into a CpuState register
    first, so the direct-literal regex matched exactly none of them.

    Evidence tier: SOURCE_SHAPE / STATICALLY_SUPPORTED throughout. These tests
    read source, they do not run a guest.
    """

    def setUp(self) -> None:
        self.source = HLE_C.read_text(encoding="utf-8")

    # ---- A: the real sites ------------------------------------------------
    def test_the_real_runtime_sync_sites_are_detected(self) -> None:
        found = extract_hle_guest_sites(self.source)
        for address, role in RUNTIME_SYNC_CALLBACK_SITES.items():
            with self.subTest(address=hex(address), role=role):
                self.assertIn(address, found,
                              f"0x{address:08x} ({role}) is title coupling in a generic "
                              "PSP handler and must be visible to the gate")
                shapes = {shape for _line, _fn, shape in found[address]}
                self.assertTrue(shapes & {"bound_local", "cpu_state_register"},
                                f"0x{address:08x} was found only through {sorted(shapes)}; "
                                "this test would then pass without the indirect grammar")

    def test_the_real_runtime_sync_sites_are_inventoried(self) -> None:
        inventoried = hle_inventoried_addresses()
        for address, role in RUNTIME_SYNC_CALLBACK_SITES.items():
            with self.subTest(address=hex(address), role=role):
                self.assertIn(address, inventoried)

    def test_the_sites_are_attributed_to_their_real_enclosing_function(self) -> None:
        """The historical h_*-only function regex attributed everything in this
        helper to whichever handler happened to precede it."""
        found = extract_hle_guest_sites(self.source)
        for address in RUNTIME_SYNC_CALLBACK_SITES:
            functions = {fn for _line, fn, _shape in found[address]}
            self.assertEqual(functions, {"ensure_runtime_sync_callbacks"},
                             f"0x{address:08x} attributed to {sorted(functions)}")

    def test_the_direct_regex_alone_finds_none_of_them(self) -> None:
        """FAILING-BEFORE PROOF: run the pre-2026-08-21 direct-literal scan over
        the same source and show it matched none of the eight. If this ever
        starts finding them, the indirect grammar is no longer load-bearing and
        the tests above have become vacuous."""
        direct = set()
        for match in HLE_GUEST_ADDRESS_RE.finditer(self.source):
            direct.add(int(match.group(1) or match.group(2), 16))
        self.assertEqual(direct & set(RUNTIME_SYNC_CALLBACK_SITES), set(),
                         "the direct-literal shapes now reach the indirect sites")

    # ---- B: const-local address propagation --------------------------------
    def test_a_const_local_used_as_an_address_base_is_detected(self) -> None:
        snippet = (
            "static void h_Fake(CpuState *s) {\n"
            "    const uint32_t base = 0x08123400u;\n"
            "    MEM_W32(base + 0x20u, 1u);\n"
            "}\n"
        )
        found = extract_hle_guest_sites(snippet)
        self.assertIn(0x08123400, found)
        self.assertEqual(found[0x08123400][0][2], "bound_local")

    # ---- C: hidden callback stored through guest memory --------------------
    def test_a_literal_stored_into_guest_memory_as_a_value_is_detected(self) -> None:
        """The shape that hid the six wrapper entry points: the literal never
        appears in an ADDRESS position, only as the value being written."""
        snippet = (
            "static void h_Fake(CpuState *s) {\n"
            "    const uint32_t base = 0x08123400u;\n"
            "    uint32_t cb = 0x00123450u;\n"
            "    MEM_W32(base + 0x34u, cb);\n"
            "}\n"
        )
        found = extract_hle_guest_sites(snippet)
        self.assertIn(0x00123450, found)
        self.assertEqual(found[0x00123450][0][2], "bound_local")

    def test_a_reassigned_local_is_detected_at_each_binding(self) -> None:
        """The handler binds `enter` once per switch arm; each arm is its own
        title address and each must be inventoried separately."""
        snippet = (
            "static void h_Fake(CpuState *s) {\n"
            "    uint32_t enter = 0u;\n"
            "    switch (mode) {\n"
            "    case 0:\n"
            "        enter = 0x00123450u;\n"
            "        break;\n"
            "    case 1:\n"
            "        enter = 0x00123460u;\n"
            "        break;\n"
            "    }\n"
            "    MEM_W32(0x08123400u + 0x34u, enter);\n"
            "}\n"
        )
        found = extract_hle_guest_sites(snippet)
        self.assertIn(0x00123450, found)
        self.assertIn(0x00123460, found)

    # ---- D: CpuState register assignment -----------------------------------
    def test_a_literal_assigned_to_a_cpu_state_register_is_detected(self) -> None:
        for statement in ("    call.r[4] = 0x00123450u;",
                          "    s->r[4] = 0x00123450u;"):
            with self.subTest(statement=statement.strip()):
                snippet = f"static void h_Fake(CpuState *s) {{\n{statement}\n}}\n"
                found = extract_hle_guest_sites(snippet)
                self.assertIn(0x00123450, found)
                self.assertEqual(found[0x00123450][0][2], "cpu_state_register")

    # ---- E/F: inventory drift ----------------------------------------------
    def test_a_missing_inventory_entry_fails_the_gate(self) -> None:
        """Drop one of the eight from the inventory and the coverage gate must
        fail. Asserted through the same set arithmetic the gate uses."""
        found = set(extract_hle_guest_addresses(self.source))
        for address in RUNTIME_SYNC_CALLBACK_SITES:
            with self.subTest(address=hex(address)):
                thinned = hle_inventoried_addresses() - {address}
                self.assertEqual(found - thinned, {address},
                                 "removing an inventory entry must leave exactly that "
                                 "address uncovered")

    def test_a_stale_inventory_entry_fails_the_gate(self) -> None:
        found = set(extract_hle_guest_addresses(self.source))
        invented = 0x08FEDCB0
        self.assertNotIn(invented, found)
        stale = (hle_inventoried_addresses() | {invented}) - found
        self.assertEqual(stale, {invented})

    # ---- G: the grammar itself must not go silently dead --------------------
    def test_the_indirect_grammar_is_not_vacuous(self) -> None:
        """BLIND-SPOT REGRESSION: if extract_hle_indirect_sites stops matching --
        a regex edit, a formatting change in hle.c -- every test above would pass
        for the wrong reason on an inventory that still lists the addresses."""
        indirect = extract_hle_indirect_sites(self.source)
        self.assertGreaterEqual(len(indirect), 8,
                                "the indirect grammar matched almost nothing; it has "
                                "probably gone dead and its coverage tests with it")
        self.assertTrue(set(RUNTIME_SYNC_CALLBACK_SITES) <= set(indirect))
        shapes = {shape for sites in indirect.values() for _l, _f, shape in sites}
        self.assertEqual(shapes, {"bound_local", "cpu_state_register"},
                         "both indirect shapes must still be live")

    def test_function_spans_do_not_collapse(self) -> None:
        """The grammar is per-function: if span detection degraded to one giant
        span, a binding in one function would pair with a use in another and the
        false-positive guard below would stop meaning anything."""
        spans = hle_function_spans(self.source.splitlines())
        self.assertGreater(len(spans), 100, "function-span detection collapsed")
        names = {name for name, _s, _e in spans}
        self.assertIn("ensure_runtime_sync_callbacks", names)
        self.assertIn("h_DisplaySetMode", names)

    # ---- H: no false-positive explosion ------------------------------------
    def test_unrelated_constants_do_not_become_coupling(self) -> None:
        snippet = (
            "static void h_Fake(CpuState *s) {\n"
            "    uint32_t prev = 0xFFFFFFFFu;\n"          # sentinel, unaligned
            "    s->r[3] = 0xFFFFFFFFu;\n"                # errno, unaligned
            "    s->r[24] = 0xDEADBEEFu;\n"               # poison, unaligned
            "    uint32_t len = 0x000646f0u;\n"           # aligned, but never coupled
            "    uint32_t flags = 0x00081000u;\n"         # aligned, but never coupled
            "    fprintf(stderr, \"%u %u %u\", prev, len, flags);\n"
            "}\n"
        )
        self.assertEqual(extract_hle_guest_sites(snippet), {},
                         "a literal that never reaches guest state is not coupling")

    def test_the_real_file_produces_no_unclassified_indirect_address(self) -> None:
        """The grammar's false-positive rate on the real file is zero: every
        address it reports is either one of the eight or an address the direct
        scan already inventoried."""
        indirect = set(extract_hle_indirect_sites(self.source))
        unexpected = indirect - set(RUNTIME_SYNC_CALLBACK_SITES) - hle_inventoried_addresses()
        self.assertEqual(unexpected, set(),
                         f"unclassified: {sorted(hex(a) for a in unexpected)}")

    def test_the_alignment_rule_is_what_excludes_the_sentinels(self) -> None:
        """Document the limit honestly: the indirect shapes admit only 4-byte
        aligned literals, and that -- not a magnitude ceiling, which this gate
        rejects on principle -- is what keeps errno/poison values out. An
        UNALIGNED guest address reached indirectly is a known gap."""
        aligned = ("static void h_Fake(CpuState *s) {\n"
                   "    s->r[4] = 0x08123400u;\n}\n")
        unaligned = ("static void h_Fake(CpuState *s) {\n"
                     "    s->r[4] = 0x08123401u;\n}\n")
        self.assertIn(0x08123400, extract_hle_guest_sites(aligned))
        self.assertEqual(extract_hle_guest_sites(unaligned), {})

    # ---- I: a ninth hidden callback must fail the gate ---------------------
    def test_a_ninth_hidden_callback_literal_fails_the_gate(self) -> None:
        """MUTATION: add a ninth hidden wrapper target to the real handler the
        same way the existing six are written, and the coverage gate must refuse
        it. This is the property the whole slice exists for."""
        contaminated = self.source.replace(
            "        enter = 0x000824c0u;",
            "        enter = 0x000824c0u;\n        leave = 0x0009abc0u;",
            1,
        )
        self.assertNotEqual(contaminated, self.source, "mutation anchor not found")
        found = extract_hle_guest_addresses(contaminated)
        self.assertIn(0x0009ABC0, found)
        missing = set(found) - hle_inventoried_addresses()
        self.assertEqual(missing, {0x0009ABC0},
                         "a newly hidden callback literal must be the one thing the "
                         "coverage gate reports as uninventoried")

    # ---- the inventory entry itself ----------------------------------------
    def test_the_inventory_entry_states_its_retirement_shape(self) -> None:
        """The eight are PROFILE_OWNED_CONFIGURATION in shape but are NOT typed
        configuration today. The entry has to say both, and has to say that the
        mode-keyed pairs must not be flattened into scalar bindings."""
        group = next(g for g in compat_overrides.HLE_GUEST_ADDRESS_GROUPS
                     if g["name"] == "runtime_sync_callback_config")
        self.assertEqual(group["title2_bucket"], "EXPLICIT_COMPATIBILITY_OVERRIDE")
        self.assertEqual(set(group["addresses"]), set(RUNTIME_SYNC_CALLBACK_SITES))
        self.assertIn("PROFILE_OWNED_CONFIGURATION", group["retirement"])
        self.assertIn("not be flattened", group["retirement"].replace("NOT", "not"))
        # The claim must stay a source claim: no route was run for it. SOURCE_SHAPE is
        # the label AGENTS.md section 9 defines for "static structure/text/emission
        # assertion only" -- the inventory must not invent a competing vocabulary.
        self.assertEqual(group["evidence_tier"], "SOURCE_SHAPE")
        self.assertIn("SOURCE_SHAPE", group["evidence"])

if __name__ == "__main__":
    unittest.main()
