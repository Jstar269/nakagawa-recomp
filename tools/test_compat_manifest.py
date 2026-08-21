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

SCANNER CONTRACT: the extractor is a regex over a fixed set of direct-literal
shapes (MEM_R*/MEM_W*, sr_r32/sr_w32, dispatch, ge_call_guest*, and VRAM-window
``return`` literals).  It fails closed for supported direct-literal guest
memory/dispatch/call shapes.  It does NOT claim universal syntactic
fail-closedness: a site that does not flow through one of those exact shapes
(e.g. an address computed at runtime, a struct initializer, or a literal in a
value position) is outside this gate's proven contract and must be kept
visible by the other inventory mechanisms.  New absolute guest address sites
are mechanically enumerable precisely because they must appear through one of
the supported shapes to be admitted; the census test pins the current count so
a shape-set change is a deliberate, reviewed act.

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

HLE_FUNCTION_RE = re.compile(r"static [a-zA-Z0-9_ \*]+\b(h_[a-zA-Z0-9_]+)\s*\(\s*CpuState \*s\s*\)")


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
        if fn:
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
        38 distinct addresses across 50 sites in src/rt/hle.c, with every site
        inventoried and no stale inventory entries.  If the corrected scanner
        legitimately changes these counts (for example after src/rt/hle.c
        changes land from #91/#92), update the census and this assertion
        together -- never silently."""
        found = extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8"))
        self.assertEqual(len(found), 38,
                         "distinct-address census drifted from the reconciled 38; "
                         "recompute the census deliberately if the scanner "
                         "legitimately changed")
        self.assertEqual(sum(len(v) for v in found.values()), 50,
                         "site-count census drifted from the reconciled 50; "
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


if __name__ == "__main__":
    unittest.main()
