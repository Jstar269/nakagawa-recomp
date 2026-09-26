#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""Behavioral + wiring coverage for the guest code-address dispatch table (issue #45).

Layer 1 (behavioral): compile and run ``src/rt/dispatch_selftest.c`` against the real
primitives in ``src/rt/dispatch_table.h`` -- register/look up address 0 as a first-class
key, hash collisions involving 0 (both orders), L1 caching, re-registration, and the
proof that a real function at address 0 executes while an unregistered lookup does not.

Layer 2 (wiring/consistency): source checks that recomp.c uses the shared header rather
than a private copy, and that the header carries occupancy in a dedicated ``state`` field
(never inferring "empty" from ``addr == 0``), so the #45 defect cannot silently return.
"""

import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RT = ROOT / "src" / "rt"
SELFTEST_C = RT / "dispatch_selftest.c"
DISPATCH_H = (RT / "dispatch_table.h").read_text(encoding="utf-8")
RECOMP_C = (RT / "recomp.c").read_text(encoding="utf-8")
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")


def _strip_comments(src: str) -> str:
    """Drop /* ... */ and // ... comments so structural checks test code, not prose."""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.DOTALL)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


DISPATCH_H_CODE = _strip_comments(DISPATCH_H)


def _has_retired_target_swallow(source: str) -> bool:
    """Detect the representative broad predicate mutation from Issue #362."""
    code = _strip_comments(source)
    return bool(re.search(
        r"target\s*&\s*0xff000000u?\s*\)\s*==\s*0x(?:33|44|55|88)000000u?",
        code,
    ))


def _exact_hook_helpers(code: str) -> list[str]:
    """Function identifiers referenced by the live g_exact_hooks[] table entries."""
    m = re.search(r"static const DispatchHook g_exact_hooks\[\] = \{(.*?)\n\};", code, re.S)
    if not m:
        return []
    return re.findall(r'"[^"]*"\s*,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\}', m.group(1))


def _c_function_body(code: str, name: str) -> str | None:
    """Comment-stripped body of `static int <name>(...) { ... }`, or None."""
    m = re.search(r"static\s+int\s+" + re.escape(name) + r"\s*\([^)]*\)\s*\{", code)
    if not m:
        return None
    depth = 0
    for i in range(m.end() - 1, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[m.end():i]
    return None


def _diagnostic_hook_violations(code: str) -> list[str]:
    """Structural contract violations for surviving g_exact_hooks[] helpers (#362).

    A surviving exact-hook helper must be diagnostic/read-only: it may log, but
    it must not look the target up itself (self-delegation either double-runs a
    registered body or pre-empts the authoritative lookup) and must not return 0,
    because hook return 0 is CONSUMED -- apparent success without the ordinary
    dispatch path, which is exactly the pre-revision HFILL/FMT defect.
    """
    violations: list[str] = []
    for fn in _exact_hook_helpers(code):
        body = _c_function_body(code, fn)
        if body is None:
            violations.append(f"{fn}: definition not found")
            continue
        if "sr_lookup" in body:
            violations.append(f"{fn}: self-delegates through sr_lookup")
        if re.search(r"return\s+0\s*;", body):
            violations.append(f"{fn}: consumes the dispatch (return 0)")
        if not re.search(r"return\s+1\s*;", body):
            violations.append(f"{fn}: does not fall through (missing return 1)")
    return violations


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestDispatchSelftestC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        assert CC is not None
        cls.tmp = tempfile.mkdtemp(prefix="dispatchc_")
        cls.exe = os.path.join(cls.tmp, "dispatch_selftest.exe")
        result = subprocess.run(
            [CC, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             f"-I{RT}", "-o", cls.exe, str(SELFTEST_C)],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            raise AssertionError("dispatch_selftest.c did not compile:\n" + result.stderr)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_behavioral_assertions_pass(self):
        result = subprocess.run([self.exe], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("dispatch selftest: OK", result.stdout)


class TestDispatchWiring(unittest.TestCase):
    def test_recomp_uses_shared_header(self):
        self.assertIn('#include "dispatch_table.h"', RECOMP_C)
        self.assertIn("sr_dtab_register(&g_dtab", RECOMP_C)
        self.assertIn("sr_dtab_lookup(&g_dtab", RECOMP_C)

    def test_recomp_has_no_private_table_copy(self):
        # The old in-file definitions must be gone, or the header fix would be dead.
        self.assertNotIn("DispatchEntry g_dispatch_table", RECOMP_C)
        self.assertNotIn("g_dispatch_l1[", RECOMP_C)

    def test_occupancy_is_key_independent(self):
        # The entry must have a dedicated occupancy field, and the probe/terminate must
        # key on it -- never on `addr == 0`, which was the #45 defect. Checked against
        # comment-stripped code so the prose describing the old design does not match.
        self.assertIn("state", DISPATCH_H_CODE)
        self.assertRegex(DISPATCH_H_CODE, r"st\s*==\s*0u")           # empty test is state-based
        self.assertNotRegex(DISPATCH_H_CODE, r"addr[^;]*==\s*0\b")   # never "addr == 0 => empty"

    def test_l1_is_bias_encoded(self):
        # ((slot + 1) << 32) keeps an all-zero word meaning "empty" even for (slot 0, addr 0),
        # and the old `addr != 0` L1 guard (which made address 0 uncacheable) is gone.
        self.assertIn("(h + 1u) << 32", DISPATCH_H_CODE)
        self.assertRegex(DISPATCH_H_CODE, r"pair\s*!=\s*0u")         # empty test is bias-based
        self.assertNotRegex(DISPATCH_H_CODE, r"addr\s*!=\s*0")       # the old L1 guard is gone

    def test_unconfigured_target_patterns_are_not_dispatch_hooks(self):
        # Issue #362: historical HST target shapes must reach ordinary lookup/interpreter
        # disposition. A generic build may not swallow null, data-looking, resource-handle,
        # unresolved-PLT or module-table values as successful returns.
        recomp_code = _strip_comments(RECOMP_C)
        exact = re.search(r"static const DispatchHook g_exact_hooks\[\] = \{(.*?)\n\};", recomp_code, re.S)
        ranges = re.search(r"static const DispatchHook g_range_hooks\[\] = \{(.*?)\n\};", recomp_code, re.S)
        self.assertIsNotNone(exact)
        self.assertIsNotNone(ranges)
        hook_tables = (exact.group(1) if exact else "") + (ranges.group(1) if ranges else "")
        retired_names = ("NULL_CALL_A", "NULL_CALL_B", "RESOURCE_HANDLE", "SCEDMAC",
                         "MODTABLE_WALK", "_REENT_DATA", "MOD_STUB", "PLT_TRAMP",
                         "INIT_LANG", "HINSERT", "hook_null_call", "hook_resource_handle",
                         "hook_sceDmac_string", "hook_modtable_walk", "hook_mod_stub",
                         "hook_plt_unimpl", "hook_init_lang", "hook_hash_insert_guard")
        for name in retired_names:
            self.assertNotIn(name, hook_tables)
            self.assertNotIn(name, recomp_code)
        self.assertNotIn("sr_dtab_resolve_computed", DISPATCH_H)
        self.assertNotIn("sr_dtab_resolve_computed", RECOMP_C)
        self.assertIn("RecompFn fn = sr_lookup(target);", RECOMP_C)

    def test_hostile_target_matrix_is_source_owned(self):
        isolation = (ROOT / "src" / "rt" / "dispatch_isolation_selftest.c").read_text(encoding="utf-8")
        self.assertIn("0x33000010u", isolation)
        self.assertIn("test_historical_target_shapes_fail_closed", isolation)

    def test_broad_swallow_mutation_is_detected(self):
        # Mutation proof: inserting one representative old-style range predicate into
        # the runtime source must be rejected by the same structural tripwire used for
        # the real tree. This keeps the regression sensitive to reintroducing the bug,
        # rather than merely checking that today's table happens to be empty.
        mutated = RECOMP_C + "\\nstatic int mutated(CpuState *s, uint32_t target) {\\n" \
            "if ((target & 0xff000000u) == 0x33000000u) { s->r[2] = 0; return 0; }\\n" \
            "return 1; }\\n"
        self.assertTrue(_has_retired_target_swallow(mutated))
        self.assertFalse(_has_retired_target_swallow(RECOMP_C))

    def test_exact_hook_helpers_are_diagnostic_fallthrough_only(self):
        # Every surviving g_exact_hooks[] helper must be diagnostic/read-only:
        # no internal sr_lookup self-delegation and no consume (return 0), so an
        # unregistered exact key always continues through normal dispatch and a
        # registered one executes exactly once at the authoritative lookup.
        code = _strip_comments(RECOMP_C)
        helpers = set(_exact_hook_helpers(code))
        self.assertEqual(
            helpers,
            {
                "hook_log_alloc_req",
                "hook_log_free_req",
                "hook_hash_fill_trace",
                "hook_fmt_trace",
                "hook_thunk_call_trace",
                "hook_plt_walk",
            },
        )
        self.assertEqual(_diagnostic_hook_violations(code), [])

    def test_self_delegating_consuming_helper_is_detected(self):
        # Tripwire sensitivity: a reintroduced helper that looks the target up
        # itself and consumes the dispatch -- the pre-revision HFILL/FMT shape,
        # under any name -- must be flagged as both a live table entry and a
        # violating helper body, while the real tree stays clean.
        code = _strip_comments(RECOMP_C)
        table_open = "static const DispatchHook g_exact_hooks[] = {\n"
        self.assertIn(table_open, code)
        mutant = code.replace(
            table_open,
            table_open + '    { 0x0000deadu, 0xFFFFFFFFu, "MUTANT", hook_mutant },\n',
            1,
        )
        mutant += (
            "\nstatic int hook_mutant(CpuState *s, uint32_t target) {\n"
            "    RecompFn f = sr_lookup(target);\n"
            "    if (f) f(s);\n"
            "    return 0;\n"
            "}\n"
        )
        violations = _diagnostic_hook_violations(mutant)
        self.assertTrue(any(v.startswith("hook_mutant:") for v in violations), violations)
        self.assertEqual(_diagnostic_hook_violations(code), [])


if __name__ == "__main__":
    unittest.main()
