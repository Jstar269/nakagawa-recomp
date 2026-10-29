#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

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
    """Detect the representative broad predicate mutation retired under #363."""
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
    """Structural contract violations for surviving g_exact_hooks[] helpers (#363).

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


def _has_generic_dispatch_hook_table(code: str) -> bool:
    code = _strip_comments(code)
    return bool(re.search(r"\b(?:DispatchHook|g_exact_hooks|g_range_hooks)\b", code))


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
        # Compatibility debt (#363): historical HST target shapes must reach ordinary lookup/interpreter
        # disposition. A generic build may not swallow null, data-looking, resource-handle,
        # unresolved-PLT or module-table values as successful returns.
        recomp_code = _strip_comments(RECOMP_C)
        exact = re.search(r"static const DispatchHook g_exact_hooks\[\] = \{(.*?)\n\};", recomp_code, re.S)
        ranges = re.search(r"static const DispatchHook g_range_hooks\[\] = \{(.*?)\n\};", recomp_code, re.S)
        self.assertIsNone(exact)
        self.assertIsNone(ranges)
        retired_names = ("NULL_CALL_A", "NULL_CALL_B", "RESOURCE_HANDLE", "SCEDMAC",
                         "MODTABLE_WALK", "_REENT_DATA", "MOD_STUB", "PLT_TRAMP",
                         "INIT_LANG", "HINSERT", "hook_null_call", "hook_resource_handle",
                         "hook_sceDmac_string", "hook_modtable_walk", "hook_mod_stub",
                         "hook_plt_unimpl", "hook_init_lang", "hook_hash_insert_guard")
        for name in retired_names:
            self.assertNotIn(name, recomp_code)
        self.assertNotIn("sr_dtab_resolve_computed", DISPATCH_H)
        self.assertNotIn("sr_dtab_resolve_computed", RECOMP_C)
        self.assertIn("RecompFn fn = sr_lookup(target);", RECOMP_C)

    def test_generic_dispatch_has_no_title_hook_tables_or_traversals(self):
        self.assertFalse(_has_generic_dispatch_hook_table(RECOMP_C),
                         "generic dispatch must not traverse a title hook table")
        mutant = RECOMP_C + "\nstatic const DispatchHook g_range_hooks[] = { { 0, 0, 0, 0 } };\n"
        self.assertTrue(_has_generic_dispatch_hook_table(mutant),
                        "hook-table regression guard did not detect its mutation")

    def test_hostile_target_matrix_is_source_owned(self):
        isolation = (ROOT / "src" / "rt" / "dispatch_isolation_selftest.c").read_text(encoding="utf-8")
        self.assertIn("0x33000010u", isolation)
        self.assertIn("test_historical_target_shapes_fail_closed", isolation)

    def test_rejections_name_the_semantic_boundary_and_tracker(self):
        self.assertIn("SEMANTIC_BOUNDARY: %s; in the works: #308", RECOMP_C)
        self.assertIn('return "target-not-executable";', RECOMP_C)
        self.assertIn('getenv("SR_BOOT_EVENT_FILE")', RECOMP_C)
        # One recorder shapes every stop line; each stop names the issue that tracks lifting it.
        self.assertIn(
            "BOOT_EVENT phase=stop reason=semantic-boundary boundary=%s issue=%u",
            RECOMP_C,
        )
        self.assertIn("dispatch_record_semantic_stop_issue(boundary, 308u);", RECOMP_C)
        self.assertIn("dispatch_record_semantic_stop_issue(boundary, 855u);", RECOMP_C)
        self.assertIn(
            "dispatch_record_semantic_stop(dispatch_semantic_boundary_name(interp_result));",
            RECOMP_C,
        )

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

    def test_exact_hook_helpers_are_removed(self):
        code = _strip_comments(RECOMP_C)
        helpers = set(_exact_hook_helpers(code))
        self.assertEqual(helpers, set())
        self.assertFalse(_has_generic_dispatch_hook_table(code))
        self.assertEqual(_diagnostic_hook_violations(code), [])

    def test_self_delegating_consuming_helper_is_detected(self):
        # Tripwire sensitivity: a reintroduced exact hook table must be detected,
        # regardless of the table entry name or whether its handler looks harmless.
        code = _strip_comments(RECOMP_C)
        mutant = code + (
            "\nstatic const DispatchHook g_exact_hooks[] = {\n"
            '    { 0x0000deadu, 0xFFFFFFFFu, "MUTANT", hook_mutant },\n};\n'
        )
        self.assertTrue(_has_generic_dispatch_hook_table(mutant))
        self.assertFalse(_has_generic_dispatch_hook_table(code))
        self.assertEqual(_diagnostic_hook_violations(code), [])


class ZeroBasedOffsetZeroPointers(unittest.TestCase):
    """#1375: a zero-based image must not hold a code pointer to its own offset 0.

    Generated code and the interpreter stop a computed jr/jalr whose target is 0 as a
    null code pointer. That is exact only because codegen refuses a relocatable module
    placed at base 0 whose relocations take the address of its offset 0, which the
    loader would write as 0. These synthetic modules (built with the runtime loader
    suite's fixture builder) pin which relocation forms take that address.
    """

    @classmethod
    def setUpClass(cls):
        import sys
        sys.path.insert(0, str(ROOT / "tools"))
        import codegen
        import test_prx_loader_cleanroom as fixture
        cls.codegen = codegen
        cls.fixture = fixture

    def module(self, words, relocs, vaddr=0x0, e_type=None):
        fx = self.fixture
        data, offs, memsz = fx.basic_seg_with_words(words, vaddr=vaddr)
        module = fx.Module() if e_type is None else fx.Module(e_type=e_type)
        module.add_seg(vaddr, data, memsz, flags=7)
        if relocs:
            module.add_reloc(fx.PT_REL_A, b"".join(
                fx.reloc_a(offs[index], kind) for index, kind in relocs))
        return module.build(), [vaddr + off for off in offs]

    def sites(self, blob, base):
        return self.codegen.zero_based_offset0_pointers(blob, base, "<synthetic>")

    def test_data_word_pointer_to_offset0_is_found_at_base_zero_only(self):
        # .word 0 relocated as an absolute pointer to offset 0 (format A kind 2).
        blob, addrs = self.module([0x00000000], [(0, 2)])
        self.assertEqual(self.sites(blob, 0), [(addrs[0], "word32")])
        self.assertEqual(self.sites(blob, None), [(addrs[0], "word32")])
        # At a nonzero base the same pointer is that base, never 0: no ambiguity.
        self.assertEqual(self.sites(blob, 0x08804000), [])

    def test_hi16_lo16_pair_forming_offset0_is_found(self):
        # lui a0, %hi(0) ; addiu a0, a0, %lo(0): the hi16 record carries the full address.
        blob, addrs = self.module([0x3C040000, 0x24840000], [(0, 5), (1, 1)])
        self.assertEqual(self.sites(blob, 0), [(addrs[0], "hi16")])

    def test_lone_lo16_from_zero_forming_offset0_is_found(self):
        # addiu a0, zero, %lo(0) with no hi16 half.
        blob, addrs = self.module([0x24040000], [(0, 1)])
        self.assertEqual(self.sites(blob, 0), [(addrs[0], "lo16")])

    def test_pointers_elsewhere_and_static_calls_are_not_pointers_to_offset0(self):
        # .word 8 (a pointer to offset 8), jal 0 (a static call), lw a0, 0(a1) relocated.
        blob, _ = self.module([0x00000008, 0x0C000000, 0x8CA40000], [(0, 2), (1, 4), (2, 1)])
        self.assertEqual(self.sites(blob, 0), [])

    def test_fixed_address_image_has_no_relocations_to_classify(self):
        blob, _ = self.module([0x00000000], [], vaddr=0x08804000, e_type=2)
        self.assertEqual(self.sites(blob, 0x08804000), [])

    def test_codegen_refuses_the_ambiguous_image_before_analysis(self):
        import contextlib
        import io
        blob, addrs = self.module([0x00000000], [(0, 2)])
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "zero_based.prx"
            image.write_bytes(blob)
            stderr = io.StringIO()
            with contextlib.redirect_stderr(stderr):
                rc = self.codegen.main(["codegen.py", str(image), str(Path(tmp) / "out.c"),
                                        "--base=0"])
            self.assertEqual(rc, 2)
            self.assertIn("#1375", stderr.getvalue())
            self.assertIn(f"word32 at 0x{addrs[0]:08x}", stderr.getvalue())
            self.assertFalse((Path(tmp) / "out.c").exists())

    def test_runtime_refuses_computed_null_at_the_transfer(self):
        code = _strip_comments(RECOMP_C)
        self.assertIn("void sr_null_code_pointer(CpuState *s, uint32_t transfer_pc)", code)
        self.assertIn('dispatch_record_semantic_stop_issue("null-code-pointer", 1375u);', code)
        self.assertIn('case SR_GUEST_INTERP_NULL_CODE_POINTER: return "null-code-pointer";', code)
        self.assertIn("#1375", DISPATCH_H)
        self.assertNotIn("#20/#45", DISPATCH_H)


if __name__ == "__main__":
    unittest.main()
