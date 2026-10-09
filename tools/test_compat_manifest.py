# SPDX-License-Identifier: GPL-3.0-or-later

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

TEMPORARY-PATCH DEBT GATE (issue #363).  A `temporary_compatibility_patch` is
by definition a patch over an open bug, so the manifest also has to say who
owns it, when it is done, and what pins it.  TemporaryCompatibilityDebtGateTests
fails any such entry whose owner_issue does not lead with a live GitHub issue
number, whose retirement condition is empty, or whose test names no regression
-- including the placeholder spellings ("n/a", "TBD", "-") that mean exactly
what test="none" means -- unless the entry is listed in
TEMPORARY_PATCH_TEST_WAIVERS below, the single reviewed place an untested
temporary patch is tolerated.  Every active waiver is printed on each run
(setUpModule -> active_waiver_notice), so a non-empty allowlist cannot be
invisible in a CI log, and a waiver covers the missing test only: an entry that
is also unowned or unretractable still fails.  The census it checks comes from
compat_overrides.temporary_compatibility_patches(), the same function
`python tools/compat_overrides.py --debt-census` reports; that report also
names the untested faithful_abi_bridge/hle_boundary entries, which this gate
does not cover and #363 still owns.

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

import contextlib
import inspect
import io
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import codegen
import compat_overrides
from host_stubs import HST_SIMPLE_STUBS


def _strip_c_comments_and_literals(source: str) -> str:
    """Keep C tokens while removing comments and quoted diagnostic text."""
    pattern = r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\''
    return re.sub(pattern, lambda match: " " * len(match.group()), source,
                  flags=re.DOTALL)


def _runtime_title_diagnostic_inventory() -> dict[str, set[int]]:
    """Address census used to keep title diagnostics out of production source."""
    runtime_paths = {
        "src/rt/hle.c", "src/rt/sched.c", "src/rt/recomp.c",
        "src/rt/debug.c", "src/rt/recomp.h", "src/rt/intr_conformance.h",
    }
    result = {path: set() for path in runtime_paths}
    for group in (compat_overrides.RETIRED_HLE_DIAGNOSTIC_GROUPS +
                  compat_overrides.RETIRED_DIAGNOSTIC_GROUPS):
        addresses = {int(address) for address in group["addresses"]}
        for path in runtime_paths:
            result[path].update(addresses)
    for hook in compat_overrides.RETIRED_DISPATCH_HOOKS:
        for path in runtime_paths:
            result[path].add(int(hook["address"]))
    for caller_pc in compat_overrides.RETIRED_INIT_WALKER_GUARD["caller_pcs"]:
        for path in runtime_paths:
            result[path].add(int(caller_pc))
    configured = [
        address for group in compat_overrides.HLE_TITLE_CONFIGURED_COMPAT
        for address in group["addresses"]
    ]
    configured += [item["address"] for item in compat_overrides.TITLE_CONFIGURED_SCHEDULER_COMPAT]
    configured += [item["address"] for item in compat_overrides.TITLE_CONFIGURED_DISPATCH]
    configured += [item["address"] for item in compat_overrides.SCHEDULER_HOOKS]
    all_addresses = (set(compat_overrides.RETIRED_DISPATCH_TARGETS) - {0, 0x1000}) | set(configured) | {
        0x0030a0bf, 0x0031101c, 0x0031105c,
    }
    for path in runtime_paths:
        result[path].update(all_addresses)
        # Zero is a sentinel, and 0x1000 is the generic first readable guest page
        # as well as an old HST-only probe PC. These values are not title literals.
        result[path].difference_update({0, 0x1000})
    return result


def _runtime_title_diagnostic_literals(source: str, addresses: set[int]) -> set[int]:
    code = _strip_c_comments_and_literals(source)
    values = {int(match.group(1), 16) for match in
              re.finditer(r"\b0x0*([0-9a-fA-F]{1,8})u?\b", code)}
    return values & addresses


def _runtime_has_dispatch_hook_table(source: str) -> bool:
    code = _strip_c_comments_and_literals(source)
    return bool(re.search(r"\b(?:DispatchHook|g_exact_hooks|g_range_hooks)\b", code))


def _diagnostic_function_body(descriptor: str) -> str | None:
    """Return a manifest-named C function body, or None for an invalid descriptor."""
    relative_path, separator, function = descriptor.partition(":")
    if not separator or not re.fullmatch(r"[A-Za-z_]\w*", function.strip()):
        return None
    source = _strip_c_comments_and_literals((ROOT / relative_path).read_text(encoding="utf-8"))
    name = function.strip()
    match = re.search(rf"\b{re.escape(name)}\s*\([^;{{}}]*\)\s*\{{", source)
    if not match:
        return None
    start = match.end()
    depth = 1
    for offset in range(start, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[start:offset]
    return None


def _diagnostic_function_has_side_effects(body: str) -> bool:
    """Reject guest-state, scheduler-state, or guest-control-flow mutations."""
    assignment = r"(?:\|=|&=|\^=|\+=|-=|(?<![=!<>])=(?!=)|\+\+|--)"
    patterns = (
        r"(?:->|\.)\s*(?:r\s*\[[^]]+\]|pc|flow_kind|flow_target|"
        r"cop0\s*\[[^]]+\])\s*" + assignment,
        r"\b(?:MEM_W\d+|sr_w32|guest_mem_write)\s*\(",
        r"\b(?:s_cur|s_tick|s_ntcb|s_tcb|s_sched_coro|sr_timeslice)\b\s*" + assignment,
        r"\bs_tcb\s*\[[^]]+\]\s*\.\s*\w+\s*" + assignment,
        r"\b(?:dispatch|SR_YIELD|sr_coro_switch|sched_(?:terminate|exit|wake|block|"
        r"create|delete)\w*)\s*\(",
    )
    return any(re.search(pattern, body) for pattern in patterns)

REPO_ROOT = ROOT
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
    if not m:
        return []
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


def blank_c_literals_and_comments(lines: list[str]) -> list[str]:
    """Return ``lines`` with C comments and string/char literal contents blanked.

    Line count and column positions are preserved, so brace counting over the result
    is not misled by a brace inside a string (for example a JSON writer emitting
    "{
") or a comment.
    """
    out: list[str] = []
    in_block = False
    for line in lines:
        chars = list(line)
        i, n = 0, len(chars)
        quote = ""
        while i < n:
            c = chars[i]
            nxt = chars[i + 1] if i + 1 < n else ""
            if in_block:
                if c == "*" and nxt == "/":
                    chars[i] = chars[i + 1] = " "
                    in_block = False
                    i += 2
                    continue
                chars[i] = " "
            elif quote:
                if c == "\\" and i + 1 < n:
                    chars[i] = chars[i + 1] = " "
                    i += 2
                    continue
                if c == quote:
                    quote = ""
                else:
                    chars[i] = " "
            elif c == "/" and nxt == "/":
                for j in range(i, n):
                    chars[j] = " "
                break
            elif c == "/" and nxt == "*":
                chars[i] = chars[i + 1] = " "
                in_block = True
                i += 2
                continue
            elif c in "\"'":
                quote = c
            i += 1
        out.append("".join(chars))
    return out


def hle_function_spans(lines: list[str]) -> list[tuple[str, int, int]]:
    """(name, first_line, last_line) for each static function body, 1-based.

    Brace counting, not parsing: the file is .clang-format'd, so a body opens on
    the signature line or the one after it and closes at depth zero. Braces inside
    comments and string/char literals are ignored.
    """
    code = blank_c_literals_and_comments(lines)
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
            depth += code[cursor].count("{") - code[cursor].count("}")
            if "{" in code[cursor]:
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


def hle_title_configured_addresses() -> set[int]:
    result: set[int] = set()
    for group in getattr(compat_overrides, "HLE_TITLE_CONFIGURED_COMPAT", []):
        result.update(group["addresses"])
    return result


def hle_retired_diagnostic_addresses() -> set[int]:
    result: set[int] = set()
    for group in compat_overrides.RETIRED_HLE_DIAGNOSTIC_GROUPS:
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
        synthetic = "static void h_probe(CpuState *s) {\n    MEM_R32(0x00abcdefu);\n}\n"
        self.assertIn(0x00abcdef, extract_hle_guest_addresses(synthetic))
        found = extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8"))
        self.assertFalse(set(found) & hle_retired_diagnostic_addresses())

    def test_every_hle_guest_address_is_inventoried(self):
        """CLEAN POSITIVE / contamination gate: the current hle.c passes, and any
        uninventoried title address added to it fails here."""
        found = extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8"))
        missing = set(found) & hle_retired_diagnostic_addresses()
        self.assertFalse(
            missing,
            "src/rt/hle.c reintroduced retired title diagnostic address(es): "
            + ", ".join(f"0x{a:08x} (line {found[a][0]})" for a in sorted(missing))
            )

    def test_manifest_has_no_stale_hle_entries(self):
        """CLASSIFICATION DRIFT: if an inventoried site disappears or materially
        changes shape (so the extractor no longer sees it), this fails instead of
        silently preserving stale metadata."""
        found = extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8"))
        stale = hle_retired_diagnostic_addresses() & set(found)
        self.assertEqual(stale, set(),
                         f"retired HST addresses returned to hle.c: {sorted(hex(a) for a in stale)}")

    def test_every_hle_group_has_a_valid_category_and_bucket(self):
        for group in compat_overrides.RETIRED_HLE_DIAGNOSTIC_GROUPS:
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
        for group in compat_overrides.RETIRED_HLE_DIAGNOSTIC_GROUPS:
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
        """The retired HST groups stay inventoried but are absent from generic HLE."""
        found = extract_hle_guest_addresses(HLE_C.read_text(encoding="utf-8"))
        retired = hle_retired_diagnostic_addresses() | hle_title_configured_addresses()
        self.assertEqual(set(found) & retired, set(),
                         "retired or title-configured HST addresses remain in hle.c")

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
        self.assertEqual(found, [], "generic dispatch must not retain an exact title-hook table")
        self.assertEqual(compat_overrides.DISPATCH_HOOKS, [])
        documented = compat_overrides.all_documented_addresses()
        undocumented = [(addr, name) for addr, name in found if addr not in documented]
        self.assertEqual(undocumented, [],
                          f"src/rt/recomp.c g_exact_hooks[] entries not in tools/compat_overrides.py "
                          f"(add a DISPATCH_HOOKS entry): {undocumented}")

    def test_dispatch_range_hooks_are_documented_by_name(self):
        found_names: set[str] = {name for _addr, name in extract_dispatch_hook_table("g_range_hooks")}
        # Empty is intentional for compatibility debt (#363): no generic range predicate may swallow
        # historical target-shaped values. Equality remains the parser/census guard.
        documented_names: set[str] = {str(o["name"]) for o in compat_overrides.DISPATCH_RANGE_HOOKS if "name" in o}
        self.assertEqual(found_names, documented_names,
                          f"src/rt/recomp.c g_range_hooks[] and tools/compat_overrides.py "
                          f"DISPATCH_RANGE_HOOKS disagree: {found_names ^ documented_names}")

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

    def test_new_semantic_overrides_must_have_tests(self):
        """Hardening (issue #98 #7): new semantic fixed-address overrides cannot be
        added with test=\"none\" without an explicit fail-closed exception."""
        # Grandfathered semantic codegen stubs that predate the gate (diagnostic
        # litter, not new). A new temporary_compatibility_patch must name a test.
        grandfathered_codegen = {0x0001034c, 0x001d9eb0}
        for entry in compat_overrides.CODEGEN_CUSTOM_STUBS:
            if entry["category"] != "temporary_compatibility_patch":
                continue
            if entry.get("test") != "none":
                continue
            self.assertIn(entry["address"], grandfathered_codegen,
                          f"CODEGEN_CUSTOM_STUBS {hex(entry['address'])} ({entry['name']}) "
                          "is a semantic override with test=\"none\"; add a regression or "
                          "an explicit exception")
        # HLE: EXPLICIT_COMPATIBILITY_OVERRIDE must never be test none
        for group in compat_overrides.HLE_GUEST_ADDRESS_GROUPS:
            if group["title2_bucket"] == "EXPLICIT_COMPATIBILITY_OVERRIDE":
                self.assertNotEqual(group.get("test"), "none",
                                    f"HLE group {group['name']} is EXPLICIT but has test none")
        # Migrated groups are PROFILE_OWNED_CONFIGURATION and must be tested
        for group in getattr(compat_overrides, "HLE_TITLE_CONFIGURED_COMPAT", []):
            self.assertNotEqual(group.get("test"), "none",
                                f"migrated HLE group {group['name']} must have a test")


#: Reviewed no-test allowlist for temporary compatibility patches (issue #363).
#:
#: A `temporary_compatibility_patch` may keep test="none" ONLY when it is
#: listed here, keyed by its census id from
#: compat_overrides.temporary_compatibility_patches(), and only with the owner
#: issue that owns the gap and a one-line reason why no existing test
#: exercises it.  This dict -- in reviewed test code, not in the manifest --
#: is the single place the debt gate accepts an untested behavior-changing
#: patch, so the waiver cannot be added by the entry it waives and the gap
#: stays visible in review.  A new temporary patch with test="none" and no
#: entry here fails TemporaryCompatibilityDebtGateTests.
#:
#: Empty as of 2026-09-30: every live temporary patch names an executable
#: regression (see the #363 census), so nothing needs waiving.
TEMPORARY_PATCH_TEST_WAIVERS: dict[str, dict[str, str]] = {
}

#: Live manifest collections the debt census must still scan.  Removing a name
#: here -- or renaming the collection -- would make its temporary patches
#: invisible to the gate, so it is a reviewed change rather than a silent one.
CENSUS_LIVE_COLLECTIONS = (
    "GUEST_PATCHES",
    "CODEGEN_CUSTOM_STUBS",
    "HST_ENTRY_ROLES",
    "HST_SIMPLE_STUBS",
    "DISPATCH_HOOKS",
    "DISPATCH_RANGE_HOOKS",
    "SCHEDULER_HOOKS",
    "TITLE_CONFIGURED_SCHEDULER_COMPAT",
    "TITLE_CONFIGURED_DISPATCH",
    "HLE_GUEST_ADDRESS_GROUPS",
    "HLE_TITLE_CONFIGURED_COMPAT",
)

#: Scanned collections that are deliberately not in CENSUS_LIVE_COLLECTIONS.
#: HLE_GENERIC_SITE_RULES holds site rules rather than manifest entries, and
#: DIAGNOSTIC_GROUPS is read-only by construction, so neither is a place a
#: temporary patch belongs -- but both are still scanned, so a temporary patch
#: appearing in one is reported rather than ignored.
CENSUS_NON_ENTRY_COLLECTIONS = ("HLE_GENERIC_SITE_RULES", "DIAGNOSTIC_GROUPS")

#: A retired collection is scanned (an entry labelled temporary there is
#: fail-closed debt) but its review ownership is the retirement record, not this
#: list, so the naming convention stands in for a per-name entry.
CENSUS_RETIRED_PREFIX = "RETIRED_"

def temporary_debt_failures() -> list[str]:
    """Every #363 debt-gate violation, as "census id: problem" strings.

    The defects come from compat_overrides.temporary_patch_defects, the same
    rule --debt-census reports, so the census and the gate cannot disagree.
    The gate test asserts this is empty; the mutation tests below prove it is
    not vacuous by injecting an untested temporary patch and watching it fill.
    """
    failures: list[str] = []
    for patch in compat_overrides.temporary_compatibility_patches():
        problems = compat_overrides.temporary_patch_defects(patch)
        if patch["id"] in TEMPORARY_PATCH_TEST_WAIVERS:
            # A reviewed waiver covers exactly the missing executable test; a
            # reference that does not resolve is an invented test and still fails.
            problems = [problem for problem in problems
                        if problem not in compat_overrides.WAIVABLE_TEST_DEFECTS]
        failures.extend(f"{patch['id']}: {problem}" for problem in problems)
    return failures


def active_waiver_notice() -> str:
    """The audit notice for the no-test waivers this run is tolerating.

    Printed by :func:`setUpModule` on every run, so an allowlist that has grown
    shows up in the CI log next to the gate that honoured it.  An invisible
    waiver is an unreviewed hole wearing review's clothes, and a green log is
    exactly where nobody would look for one.
    """
    if not TEMPORARY_PATCH_TEST_WAIVERS:
        return "#363 debt gate: no active TEMPORARY_PATCH_TEST_WAIVERS entries"
    lines = [
        f"#363 debt gate: {len(TEMPORARY_PATCH_TEST_WAIVERS)} active "
        "TEMPORARY_PATCH_TEST_WAIVERS entry/entries, each an untested temporary "
        "compatibility patch still on the books:",
    ]
    for patch_id in sorted(TEMPORARY_PATCH_TEST_WAIVERS):
        waiver = TEMPORARY_PATCH_TEST_WAIVERS[patch_id]
        owner = str(waiver.get("owner_issue") or "").strip() or "-"
        reason = str(waiver.get("reason") or "").strip() or "-"
        lines.append(f"    {patch_id}: owner {owner}: {reason}")
    return "\n".join(lines)


def setUpModule() -> None:
    print(active_waiver_notice())


class TemporaryCompatibilityDebtGateTests(unittest.TestCase):
    """Issue #363: no temporary compatibility patch without an owner, a
    retirement condition and a real test.

    A temporary patch papers over an open bug, so the three census facts are
    what make it debt instead of a mystery: an owner issue to retire it
    against, a condition that says when it is done, and a regression that pins
    it so removing it later proves something.  Missing any one is reported,
    never waived here -- the only escape is the reviewed
    TEMPORARY_PATCH_TEST_WAIVERS table.
    """

    def test_the_census_scans_every_live_collection(self):
        scanned = compat_overrides.manifest_collections()
        for name in CENSUS_LIVE_COLLECTIONS:
            self.assertIn(
                name, scanned,
                f"the debt census no longer scans {name}; every temporary patch in "
                "that collection would be invisible to this gate",
            )

    def test_every_scanned_collection_is_reviewed_for_the_census(self):
        """The reverse direction: a collection added to the manifest must be a
        reviewed decision.  The census scans it either way, but silence is not
        review -- without this, a new live collection of temporary patches could
        appear with no entry in the reviewed list that says someone looked."""
        scanned = compat_overrides.manifest_collections()
        for name in scanned:
            with self.subTest(collection=name):
                self.assertTrue(
                    name in CENSUS_LIVE_COLLECTIONS
                    or name in CENSUS_NON_ENTRY_COLLECTIONS
                    or name.startswith(CENSUS_RETIRED_PREFIX),
                    f"{name} is scanned by the debt census but is in neither "
                    "CENSUS_LIVE_COLLECTIONS nor CENSUS_NON_ENTRY_COLLECTIONS; "
                    "add it (or record why it is not a reviewed collection)",
                )
        for name in CENSUS_NON_ENTRY_COLLECTIONS:
            with self.subTest(non_entry_collection=name):
                self.assertIn(
                    name, scanned,
                    f"{name} is excluded from the reviewed live list but is no "
                    "longer a collection, so the exclusion is stale",
                )

    def test_the_census_is_not_vacuous(self):
        """A census that returns nothing would pass every check below for the
        wrong reason, so pin that it finds the real entries."""
        patches = compat_overrides.temporary_compatibility_patches()
        self.assertTrue(patches, "temporary compatibility debt census is empty")
        ids = [patch["id"] for patch in patches]
        self.assertEqual(len(ids), len(set(ids)), "census ids are not unique")
        # The #363 subject: the forced branches, the walk/backdrop stubs and the
        # eight static-success HST_SIMPLE_STUBS must all be inventoried.
        anchors = {patch["id"].split(":")[1] for patch in patches}
        for address in ("0x00010950", "0x00048320", "0x0001034c", "0x001d9eb0",
                        "0x00015f98", "0x0001c604"):
            self.assertIn(address, anchors, f"census lost {address}")

    def test_every_temporary_patch_has_owner_retirement_and_test(self):
        """The gate itself: unowned, unretractable or untested debt fails."""
        failures = temporary_debt_failures()
        self.assertEqual(
            failures, [],
            "temporary compatibility patch(es) missing an owner issue, a retirement "
            "condition or an executable test (fill the field, or review an entry "
            "into TEMPORARY_PATCH_TEST_WAIVERS):\n  " + "\n  ".join(failures),
        )

    def test_a_named_regression_test_must_exist(self):
        """Every reference a temporary patch names must resolve, waived or not."""
        missing = []
        for patch in compat_overrides.temporary_compatibility_patches():
            _resolved, unresolved = compat_overrides.test_references(patch.get("test"))
            missing.extend(f"{patch['id']}: {reference}" for reference in unresolved)
        self.assertEqual(
            missing, [],
            "temporary patch names a test file or Make target that does not exist: "
            + "; ".join(missing),
        )

    def test_makefile_targets_are_rule_targets_only(self):
        """Variables, recipe prose and no-op recipes are not invocable targets."""
        targets = compat_overrides.makefile_targets()
        for target in ("sched-selftest", "dispatch-isolation-selftest", "readiness"):
            self.assertIn(target, targets)
        # EMPTY/SPACE/VULKAN_SDK are := assignments; the rest are words from tab
        # recipes ($(error ...) text and the "@:" no-op).
        for not_a_target in ("EMPTY", "SPACE", "VULKAN_SDK", "needs", "validated", "@"):
            self.assertNotIn(not_a_target, targets)

    def test_a_waiver_does_not_legalize_an_invented_reference(self):
        """MUTATION: a waiver covers an untested entry, not a fabricated test path."""
        for index, test_field in enumerate(("tools/does_not_exist.py", "make EMPTY")):
            with self.subTest(test=test_field):
                address = 0x08ff0040 + 4 * index
                name = f"synthetic waived invented reference {index}"
                census_id = f"HST_SIMPLE_STUBS:0x{address:08x}:{name}"
                compat_overrides.HST_SIMPLE_STUBS.append(dict(
                    address=address, category="temporary_compatibility_patch",
                    name=name, owner_issue="#363",
                    retirement="remove the synthetic patch", test=test_field,
                ))
                TEMPORARY_PATCH_TEST_WAIVERS[census_id] = dict(
                    owner="#363", reason="synthetic waiver")
                try:
                    failures = temporary_debt_failures()
                    census = compat_overrides.format_debt_census()
                finally:
                    compat_overrides.HST_SIMPLE_STUBS.pop()
                    del TEMPORARY_PATCH_TEST_WAIVERS[census_id]
                self.assertTrue(
                    any(failure.startswith(census_id + ":") for failure in failures),
                    f"waived test={test_field!r} passed the gate",
                )
                defects = census.split("DEFECTS", 1)[-1]
                self.assertIn(census_id, defects,
                              "--debt-census DEFECTS omits an unresolvable reference")

    def test_a_temporary_patch_needs_a_resolvable_test_reference(self):
        """MUTATION: plausible prose and a nonexistent make target are not
        executable regression references, even when the deny-list has no
        matching spelling for them."""
        for index, test_field in enumerate((
            "pending",
            "covered manually",
            "make no-such-target",
        )):
            with self.subTest(test=test_field):
                mutant = dict(
                    address=0x08ff0030 + 4 * index,
                    category="temporary_compatibility_patch",
                    name=f"synthetic unresolved reference {index}",
                    owner_issue="#363",
                    retirement="remove the synthetic patch",
                    test=test_field,
                )
                compat_overrides.HST_SIMPLE_STUBS.append(mutant)
                try:
                    failures = temporary_debt_failures()
                finally:
                    compat_overrides.HST_SIMPLE_STUBS.pop()
                census_id = (
                    f"HST_SIMPLE_STUBS:0x{0x08ff0030 + 4 * index:08x}:"
                    f"synthetic unresolved reference {index}"
                )
                self.assertTrue(
                    any(failure.startswith(census_id + ":") for failure in failures),
                    f"test={test_field!r} passed without an existing tools Python "
                    "test path or Makefile target",
                )

    def test_the_no_test_allowlist_is_attributed_and_current(self):
        """A waiver without an owner or a reason is an unexplained hole, and a
        waiver for an entry that no longer exists is a stale one."""
        patches = {patch["id"]: patch for patch in
                   compat_overrides.temporary_compatibility_patches()}
        for patch_id, waiver in TEMPORARY_PATCH_TEST_WAIVERS.items():
            with self.subTest(waiver=patch_id):
                self.assertIn(
                    patch_id, patches,
                    f"stale waiver: {patch_id} is not a live temporary patch",
                )
                for key in ("owner_issue", "reason"):
                    self.assertTrue(
                        str(waiver.get(key, "")).strip(),
                        f"waiver {patch_id} must record {key}",
                    )
                self.assertTrue(
                    compat_overrides.OWNER_ISSUE_RE.match(
                        str(waiver["owner_issue"]).strip()),
                    f"waiver {patch_id} owner must lead with a live issue number",
                )
                # The waiver must be doing work: the entry really has no test.
                self.assertIn(
                    "test is none/empty",
                    compat_overrides.temporary_patch_defects(patches[patch_id]),
                    f"waiver {patch_id} covers an entry that now has a test; "
                    "delete the waiver instead of carrying a stale one",
                )

    def test_an_unowned_temporary_patch_is_reported(self):
        """NEGATIVE: the predicate fires on the shape the gate exists to catch,
        and stays quiet on a complete entry -- otherwise it is vacuous."""
        synthetic = dict(address=0x08ff0000, category="temporary_compatibility_patch",
                         name="synthetic unowned patch", test="none")
        self.assertEqual(
            sorted(compat_overrides.temporary_patch_defects(synthetic)),
            ["no retirement condition",
             "owner_issue must lead with a live GitHub issue number (e.g. #363)",
             "test is none/empty"],
        )
        complete = dict(synthetic, owner_issue="#363", retirement="remove the patch",
                        test="tools/test_compat_manifest.py")
        self.assertEqual(compat_overrides.temporary_patch_defects(complete), [])
        # An ISSUES.md anchor is a documentation pointer, not a live owner.
        pointed = dict(complete, owner_issue="ISSUES.md #5.1")
        self.assertTrue(compat_overrides.temporary_patch_defects(pointed))

    def test_a_new_untested_temporary_patch_fails_the_gate(self):
        """MUTATION: appending an untested temporary patch to a live collection
        must make the failure list non-empty -- this is the pre-fix state the
        #363 census started from."""
        mutant = dict(address=0x08ff0004, category="temporary_compatibility_patch",
                      name="synthetic untested patch", test="none")
        compat_overrides.HST_SIMPLE_STUBS.append(mutant)
        try:
            failures = temporary_debt_failures()
        finally:
            compat_overrides.HST_SIMPLE_STUBS.pop()
        matching = [f for f in failures
                    if f.startswith("HST_SIMPLE_STUBS:0x08ff0004:synthetic untested patch")]
        self.assertTrue(
            matching,
            "an untested temporary patch was added to a live collection and the "
            f"debt gate did not report it; failures were: {failures}",
        )

    def test_an_empty_or_missing_test_field_fails_the_gate(self):
        """MUTATION: test="" and a missing test field are the same gap as
        test="none" -- the field may not be blanked out instead of filled."""
        for index, mutant in enumerate((
            dict(address=0x08ff0008, category="temporary_compatibility_patch",
                 name="synthetic empty-test patch", owner_issue="#363",
                 retirement="delete the patch", test=""),
            dict(address=0x08ff000c, category="temporary_compatibility_patch",
                 name="synthetic missing-test patch", owner_issue="#363",
                 retirement="delete the patch"),
        )):
            with self.subTest(mutant=index):
                compat_overrides.HST_SIMPLE_STUBS.append(mutant)
                try:
                    failures = temporary_debt_failures()
                finally:
                    compat_overrides.HST_SIMPLE_STUBS.pop()
                prefix = f"HST_SIMPLE_STUBS:0x{mutant['address']:08x}:{mutant['name']}"
                self.assertIn(
                    f"{prefix}: test is none/empty", failures,
                    "an empty or missing test field did not fail the debt gate",
                )

    def test_a_placeholder_test_spelling_is_still_a_missing_test(self):
        """A gate that matches one magic word can be passed by relabelling the
        same gap.  "n/a", "TBD" and "-" claim exactly what test="none" claims."""
        for value in ("none", "NONE", "None.", " n/a ", "N/A", "tbd", "TBD",
                      "todo", "-", "no test", None, ""):
            with self.subTest(test=value):
                synthetic = dict(address=0x08ff0010, category="temporary_compatibility_patch",
                                 name="synthetic placeholder patch", owner_issue="#363",
                                 retirement="delete the patch", test=value)
                self.assertIn(
                    "test is none/empty", compat_overrides.temporary_patch_defects(synthetic),
                    f'test={value!r} was accepted as an executable regression',
                )
        # A real claim is still a real claim, including one that merely CONTAINS
        # a placeholder word ("make sched-selftest asserts no role is captured").
        for value in ("tools/test_codegen_profile_isolation.py",
                      "make sched-selftest (generic build asserts no role is captured)",
                      "tools/test_hle_title_isolation.py and flagship verification"):
            with self.subTest(test=value):
                synthetic = dict(address=0x08ff0014, category="temporary_compatibility_patch",
                                 name="synthetic tested patch", owner_issue="#363",
                                 retirement="delete the patch", test=value)
                self.assertEqual(compat_overrides.temporary_patch_defects(synthetic), [])

    def test_a_placeholder_temporary_patch_fails_the_gate(self):
        """MUTATION: the end-to-end form of the previous test -- a placeholder
        test field added to a live collection is reported by the gate itself."""
        mutant = dict(address=0x08ff0018, category="temporary_compatibility_patch",
                      name="synthetic placeholder patch", owner_issue="#363",
                      retirement="delete the patch", test="n/a")
        compat_overrides.HST_SIMPLE_STUBS.append(mutant)
        try:
            failures = temporary_debt_failures()
        finally:
            compat_overrides.HST_SIMPLE_STUBS.pop()
        self.assertIn(
            "HST_SIMPLE_STUBS:0x08ff0018:synthetic placeholder patch: test is none/empty",
            failures,
            f'test="n/a" passed the debt gate; failures were: {failures}',
        )

    def test_a_waiver_hides_only_the_missing_test(self):
        """A waiver is one reviewed exception, not a pass: an entry that is also
        unowned or unretractable must still fail with its waiver in place."""
        mutant = dict(address=0x08ff001c, category="temporary_compatibility_patch",
                      name="synthetic waived patch", test="none")
        census_id = "HST_SIMPLE_STUBS:0x08ff001c:synthetic waived patch"
        TEMPORARY_PATCH_TEST_WAIVERS[census_id] = dict(
            owner_issue="#363", reason="synthetic waiver under test")
        compat_overrides.HST_SIMPLE_STUBS.append(mutant)
        try:
            failures = temporary_debt_failures()
        finally:
            compat_overrides.HST_SIMPLE_STUBS.pop()
            del TEMPORARY_PATCH_TEST_WAIVERS[census_id]
        self.assertNotIn(f"{census_id}: test is none/empty", failures)
        self.assertIn(f"{census_id}: no retirement condition", failures)
        self.assertTrue(
            any(f.startswith(f"{census_id}: owner_issue must lead") for f in failures),
            f"a waiver silently covered the missing owner_issue; failures were: {failures}",
        )

    def test_the_active_waiver_audit_notice_names_every_waiver(self):
        """The allowlist is kept but printed: a waiver that only exists in the
        source is a waiver no reviewer ever sees in a CI log."""
        self.assertIn(
            "no active", active_waiver_notice(),
            "an empty allowlist must still say so, or the printer itself is "
            "unverifiable",
        )
        census_id = "HST_SIMPLE_STUBS:0x08ff0020:synthetic notice patch"
        TEMPORARY_PATCH_TEST_WAIVERS[census_id] = dict(
            owner_issue="#363", reason="synthetic waiver for the notice")
        try:
            notice = active_waiver_notice()
        finally:
            del TEMPORARY_PATCH_TEST_WAIVERS[census_id]
        self.assertIn("1 active", notice)
        self.assertIn(census_id, notice)
        self.assertIn("#363", notice)
        self.assertIn("synthetic waiver for the notice", notice)

    def test_the_census_report_is_deterministic_and_names_defects(self):
        """`--debt-census` is a report a human reads and a reviewer quotes, so
        it must be stable, must exit 0, and must say which entry is defective."""
        first = compat_overrides.format_debt_census()
        self.assertEqual(first, compat_overrides.format_debt_census())
        self.assertIn("DEFECTS: none", first)
        self.assertIn("temporary_compatibility_patch entries:", first)

        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            status = compat_overrides.main(["--debt-census"])
        self.assertEqual(status, 0)
        self.assertEqual(buffer.getvalue(), first + "\n")

        mutant = dict(address=0x08ff0024, category="temporary_compatibility_patch",
                      name="synthetic reported patch", test="n/a")
        compat_overrides.HST_SIMPLE_STUBS.append(mutant)
        try:
            reported = compat_overrides.format_debt_census()
        finally:
            compat_overrides.HST_SIMPLE_STUBS.pop()
        self.assertIn("defective entries: 1", reported)
        defect_lines = [line.strip() for line in reported.splitlines()
                        if line.strip().startswith(
                            "HST_SIMPLE_STUBS:0x08ff0024:synthetic reported patch:")]
        self.assertEqual(
            len(defect_lines), 1,
            f"the DEFECTS section did not name the injected entry once; report was:\n{reported}",
        )
        self.assertIn("test is none/empty", defect_lines[0])

    def test_the_census_report_names_untested_entries_outside_the_gate(self):
        """#363's acceptance is broader than the temporary-patch category.  The
        remaining behavior-changing categories are REPORTED -- named debt with a
        stable id -- instead of being gated here or left invisible."""
        untested = compat_overrides.untested_non_temporary_entries()
        self.assertTrue(untested, "the out-of-gate untested report is empty")
        ids = [record["id"] for record in untested]
        self.assertEqual(ids, sorted(ids), "the out-of-gate report is not ordered")
        self.assertEqual(len(ids), len(set(ids)), "out-of-gate census ids are not unique")
        for record in untested:
            with self.subTest(record=record["id"]):
                self.assertIn(record["category"],
                              compat_overrides.NON_TEMPORARY_BEHAVIOR_CHANGING_CATEGORIES)
                self.assertTrue(compat_overrides.has_no_test(record))
                self.assertNotEqual(record["category"], "temporary_compatibility_patch")
        report = compat_overrides.format_debt_census()
        self.assertIn("UNTESTED OUTSIDE THE DEBT GATE", report)
        for record in untested:
            self.assertIn(record["id"], report)


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

    Historically the census reported complete at 38/38 while an entire title
    coupling (runtime_sync) was invisible to the direct regex. The grammar was
    added to catch it. Those 8 addresses are now typed title configuration
    (issue #98) and must NOT appear in hle.c at all; the grammar itself remains
    load-bearing for future indirect sites.

    Evidence tier: SOURCE_SHAPE / STATICALLY_SUPPORTED throughout. These tests
    read source, they do not run a guest.
    """

    def setUp(self) -> None:
        self.source = HLE_C.read_text(encoding="utf-8")

    # ---- A: the migrated sites must be absent from hle.c -------------------
    def test_the_real_runtime_sync_sites_are_detected(self) -> None:
        """The 8 runtime_sync addresses were migrated to title_config and must
        NOT be found in hle.c any more (wrong-title safety). The indirect
        grammar is still exercised by the synthetic snippets below."""
        found = extract_hle_guest_sites(self.source)
        for address, role in RUNTIME_SYNC_CALLBACK_SITES.items():
            with self.subTest(address=hex(address), role=role):
                self.assertNotIn(address, found,
                                 f"0x{address:08x} ({role}) migrated to title_config; "
                                 "it must not appear in hle.c any more")

    def test_the_real_runtime_sync_sites_are_inventoried(self) -> None:
        """Migrated sites are inventoried in HLE_TITLE_CONFIGURED_COMPAT, not in
        the live HLE_GUEST_ADDRESS_GROUPS."""
        live = hle_inventoried_addresses()
        configured = hle_title_configured_addresses()
        for address, role in RUNTIME_SYNC_CALLBACK_SITES.items():
            with self.subTest(address=hex(address), role=role):
                self.assertNotIn(address, live,
                                 f"0x{address:08x} must not be in live HLE census")
                self.assertIn(address, configured,
                              f"0x{address:08x} must be in title-configured inventory")

    def test_the_sites_are_attributed_to_their_real_enclosing_function(self) -> None:
        """After migration this is a negative control: the addresses must not be
        attributed to any function in hle.c."""
        found = extract_hle_guest_sites(self.source)
        for address in RUNTIME_SYNC_CALLBACK_SITES:
            self.assertNotIn(address, found,
                             f"0x{address:08x} should not be in any hle.c function after migration")

    def test_the_direct_regex_alone_finds_none_of_them(self) -> None:
        """The 8 addresses must be absent from both direct and indirect scans."""
        found = extract_hle_guest_addresses(self.source)
        self.assertEqual(found.keys() & set(RUNTIME_SYNC_CALLBACK_SITES), set(),
                         "migrated runtime_sync addresses must not appear in hle.c")

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
        """A retired title address injected into generic HLE is detected."""
        live_example = 0x0030a000
        contaminated = self.source + "\nstatic void h_probe(void) { MEM_R32(0x0030a000u); }\n"
        found = set(extract_hle_guest_addresses(contaminated))
        self.assertIn(live_example, found)
        self.assertIn(live_example, hle_retired_diagnostic_addresses())
        for address in RUNTIME_SYNC_CALLBACK_SITES:
            with self.subTest(address=hex(address)):
                self.assertNotIn(address, extract_hle_guest_addresses(self.source))
                self.assertNotIn(address, hle_inventoried_addresses())

    def test_a_stale_inventory_entry_fails_the_gate(self) -> None:
        found = set(extract_hle_guest_addresses(self.source))
        invented = 0x08FEDCB0
        self.assertNotIn(invented, found)
        stale = (hle_inventoried_addresses() | {invented}) - found
        self.assertEqual(stale, {invented})

    # ---- G: the grammar itself must not go silently dead --------------------
    def test_the_indirect_grammar_is_not_vacuous(self) -> None:
        """The grammar remains live on a synthetic indirect address shape."""
        indirect = extract_hle_indirect_sites(self.source)
        self.assertEqual(indirect, {}, "title-specific indirect address remains in hle.c")
        synthetic = ("static uint32_t h_Fake(CpuState *s) {\n"
                     "    uint32_t base = 0x08123400u;\n"
                     "    return MEM_R32(base);\n}\n")
        self.assertIn(0x08123400, extract_hle_indirect_sites(synthetic))

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
        """No indirect HST coupling remains in generic hle.c."""
        indirect = set(extract_hle_indirect_sites(self.source))
        expected_indirect: set[int] = set()
        self.assertEqual(indirect, expected_indirect,
                         f"unclassified indirect in hle.c: {sorted(hex(a) for a in indirect)}")
        self.assertTrue(expected_indirect <= hle_retired_diagnostic_addresses())

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
        """MUTATION: add a new direct MEM_W32 at a fresh address; the gate must refuse it."""
        contaminated = self.source.replace(
            "DISPLAY_SET_MODE: display bringup not configured",
            "DISPLAY_SET_MODE: display bringup not configured\n    MEM_W32(0x0009abc0u, 1u);",
            1,
        )
        self.assertNotEqual(contaminated, self.source, "mutation anchor not found")
        found = extract_hle_guest_addresses(contaminated)
        self.assertIn(0x0009ABC0, found)
        self.assertIn(0x0009ABC0, found,
                      "the address extractor stopped detecting a new guest-memory access")

    # ---- the inventory entry itself ----------------------------------------
    def test_the_inventory_entry_states_its_retirement_shape(self) -> None:
        """The eight migrated groups are now PROFILE_OWNED_CONFIGURATION via
        title_config; the entry must say so and that pairs are not flattened."""
        group = next(g for g in compat_overrides.HLE_TITLE_CONFIGURED_COMPAT
                     if g["name"] == "runtime_sync_callback_config")
        self.assertEqual(group["title2_bucket"], "PROFILE_OWNED_CONFIGURATION")
        self.assertEqual(set(group["addresses"]), set(RUNTIME_SYNC_CALLBACK_SITES))
        self.assertIn("typed", group["retirement"].lower())
        self.assertIn("not be flattened", group["retirement"].replace("NOT", "not"))
        # The migrated entry's evidence is now title_config gated, not a direct hle.c shape
        self.assertEqual(group["evidence_tier"], "SOURCE_SHAPE")
        self.assertIn("title_config", group["evidence"].lower())


class GenericRuntimeTitleAddressGuardTests(unittest.TestCase):
    def test_generic_production_source_has_no_title_address_literals(self) -> None:
        inventory = _runtime_title_diagnostic_inventory()
        violations: list[str] = []
        for relative_path, addresses in inventory.items():
            source = (ROOT / relative_path).read_text(encoding="utf-8")
            found = _runtime_title_diagnostic_literals(source, addresses)
            violations.extend(f"{relative_path}:0x{address:08x}"
                              for address in sorted(found))
        self.assertEqual(violations, [],
                         "title-specific runtime addresses belong in validated title "
                         "configuration; diagnostics use generic SR_TRACE_PC/SR_WATCH: "
                         + ", ".join(violations))

        # Mutation proof: a new literal in each production source shape is rejected by
        # the same census, even when its address is only used by diagnostic code.
        for relative_path, address in (
                ("src/rt/hle.c", 0x0030a000),
                ("src/rt/sched.c", 0x00025a50),
                ("src/rt/recomp.c", 0x00000fdc),
                ("src/rt/debug.c", 0x0030a000),
                ("src/rt/recomp.h", 0x0030a000),
                ("src/rt/intr_conformance.h", 0x00331b80)):
            source = (ROOT / relative_path).read_text(encoding="utf-8")
            contaminated = source + f"\nstatic void title_probe(void) {{ (void)0x{address:08x}u; }}\n"
            found = _runtime_title_diagnostic_literals(
                contaminated, inventory[relative_path])
            self.assertIn(address, found, relative_path)

    def test_init_walker_guard_is_title_gated_not_generic(self) -> None:
        inventory = compat_overrides.RETIRED_INIT_WALKER_GUARD
        self.assertEqual(inventory["decision"], "b")
        self.assertIn("HST only", inventory["reason"])
        source = RECOMP_C.read_text(encoding="utf-8")
        self.assertNotIn("INIT_WALKER_GUARD", source)
        # The restore in generic dispatch is armed only through the title configuration.
        self.assertIn("sr_title_config_preserve_callee_saved_at_calls()", source)
        self.assertIn("s->r[reg] = callee_saved", source)


class DiagnosticGroupConsistencyTests(unittest.TestCase):
    def test_live_diagnostic_groups_resolve_to_read_only_function_slices(self) -> None:
        for group in compat_overrides.DIAGNOSTIC_GROUPS:
            with self.subTest(group=group.get("name")):
                body = _diagnostic_function_body(group.get("source", ""))
                self.assertIsNotNone(
                    body,
                    "live DIAGNOSTIC_GROUPS entries must name src/file.c:function",
                )
                self.assertFalse(
                    _diagnostic_function_has_side_effects(body or ""),
                    "diagnostic production slice mutates guest/scheduler state or control flow",
                )

    def test_mutation_detector_covers_guest_and_scheduler_effects(self) -> None:
        read_only = "(void)MEM_R32(addr); fprintf(stderr, \"probe\");"
        mutations = (
            "s->r[16] = value;",
            "MEM_W32(addr, value);",
            "s_tick++;",
            "s_tcb[index].state = TH_READY;",
            "sched_terminate_thread(uid);",
            "dispatch(s, target);",
        )
        self.assertFalse(_diagnostic_function_has_side_effects(read_only))
        for source in mutations:
            with self.subTest(source=source):
                self.assertTrue(_diagnostic_function_has_side_effects(source))

    def test_generic_dispatch_has_no_unconditional_title_hook_table(self) -> None:
        source = (ROOT / "src/rt/recomp.c").read_text(encoding="utf-8")
        self.assertFalse(_runtime_has_dispatch_hook_table(source),
                         "generic dispatch must not traverse exact/range title-hook tables")
        mutant = source + "\nstatic const DispatchHook g_exact_hooks[] = { { 0, 0, 0, 0 } };\n"
        self.assertTrue(_runtime_has_dispatch_hook_table(mutant),
                        "hook-table guard did not detect the source mutation")

if __name__ == "__main__":
    unittest.main()
