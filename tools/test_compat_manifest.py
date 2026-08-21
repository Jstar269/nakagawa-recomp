# SPDX-License-Identifier: GPL-2.0-or-later

"""CI gate: every custom codegen stub and dispatch hook must be in the manifest.

2026-07-17. "Make false progress impossible": a game-address-specific override
that changes what the recompiled game does must be visible and classified, not
just an unremarked `if a == 0x...:` someone added during a debugging session.
This test extracts the two authoritative, structured sources of such overrides
--

  1. tools/codegen.py: GUEST_PATCHES, host_stubs.HST_SIMPLE_STUBS, and the
     per-address custom stubs between the "--- CUSTOM STUBS START/END ---"
     markers.
  2. src/rt/recomp.c: the g_exact_hooks[]/g_range_hooks[] DispatchHook tables.

-- and fails if either source contains an address/hook this manifest
(tools/compat_overrides.py) does not know about, or if the manifest lists an
address that no longer exists in either source (a stale entry masking that the
override was actually removed). Each real entry must also carry one of the
five documented categories.

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
