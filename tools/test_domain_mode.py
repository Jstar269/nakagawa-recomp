# SPDX-License-Identifier: GPL-2.0-or-later
"""LLE Phase 1 (PR 3) gates: domain mode table and the import-call seam.

Failing-before evidence (spec section 11, PR 3): on the base tree generated
imports call sr_syscall directly, there is no per-domain LLE selection, no
domain_mode.h/.c, no --lle-import-seam flag, no import_stub_text helper, and
the Makefile has no domain-mode-selftest target. Every LLE test below fails
there and passes here; the HLE-default tests pin the behavior that must not
move (default generated stubs stay byte-identical so the production-smoke,
hle-thread, sched, dispatch, and cosim gates pass unchanged).
"""

import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import codegen


DOMAIN_H = ROOT / "src" / "rt" / "domain_mode.h"
DOMAIN_C = ROOT / "src" / "rt" / "domain_mode.c"
MAKEFILE = (ROOT / "Makefile").read_text(encoding="utf-8")


class DefaultStubsUnchangedTests(unittest.TestCase):
    """Default-profile codegen still emits the historical sr_syscall stub."""

    LEGACY_STUB = (
        "void f_00001000(CpuState *s) {  /* import: ThreadManForUser nid 0x27a6b7cd */\n"
        "    sr_syscall(s, 0x27a6b7cdu);\n"
        "    sr_end(s, 0u, 0);\n}"
    )

    def test_helper_default_is_byte_identical_to_legacy_stub(self):
        self.assertEqual(
            codegen.import_stub_text(0x1000, "ThreadManForUser", 0x27A6B7CD),
            self.LEGACY_STUB,
        )

    def test_seam_is_off_by_default(self):
        self.assertFalse(codegen.LLE_IMPORT_SEAM)

    def test_default_stub_has_no_seam_call(self):
        text = codegen.import_stub_text(0x1000, "ThreadManForUser", 0x27A6B7CD)
        self.assertIn("sr_syscall(s, 0x27a6b7cdu);", text)
        self.assertNotIn("sr_import_call", text)

    def test_both_emit_sites_route_through_the_helper(self):
        source = (ROOT / "tools" / "codegen.py").read_text(encoding="utf-8")
        self.assertEqual(len(re.findall(r"text = import_stub_text\(", source)), 2)

    def test_production_fixture_recipes_do_not_opt_into_the_seam(self):
        for target in ("production-smoke:", "cosim-selftest:"):
            recipe = MAKEFILE.split(target, 1)[1].split("\n\n", 1)[0]
            self.assertNotIn("--lle-import-seam", recipe)


class ImportSeamCodegenTests(unittest.TestCase):
    """Seam-enabled stubs honor the domain table with flow propagation."""

    def test_seam_stub_calls_import_call_with_nid_and_stub_pc(self):
        text = codegen.import_stub_text(
            0x1000, "ThreadManForUser", 0x27A6B7CD, lle_import_seam=True)
        self.assertIn("sr_import_call(s, 0x27a6b7cdu, 0x00001000u);", text)
        self.assertNotIn("sr_syscall", text)

    def test_seam_stub_unwinds_on_flow(self):
        text = codegen.import_stub_text(
            0x1000, "ThreadManForUser", 0x27A6B7CD, lle_import_seam=True)
        self.assertIn("if (s->flow_kind != 0u)", text)
        self.assertIn("sr_end(s, 0u, 0); return;", text)
        self.assertTrue(text.rstrip().endswith("sr_end(s, 0u, 0);\n}"))

    def test_module_flag_drives_the_helper_default(self):
        previous = codegen.LLE_IMPORT_SEAM
        try:
            codegen.LLE_IMPORT_SEAM = True
            text = codegen.import_stub_text(0x1000, "ThreadManForUser", 0x27A6B7CD)
            self.assertIn("sr_import_call(s, 0x27a6b7cdu, 0x00001000u);", text)
        finally:
            codegen.LLE_IMPORT_SEAM = previous

    def test_seam_cli_flag_exists(self):
        source = (ROOT / "tools" / "codegen.py").read_text(encoding="utf-8")
        self.assertIn('"--lle-import-seam"', source)
        self.assertIn("LLE_IMPORT_SEAM = True", source)


class DomainSourcesTests(unittest.TestCase):
    """Source-owned domain table and seam match spec section 4."""

    def test_header_defines_domains_in_spec_order(self):
        text = DOMAIN_H.read_text(encoding="utf-8")
        for name, value in (("SR_DOMAIN_CPU", 0), ("SR_DOMAIN_BUS", 1),
                            ("SR_DOMAIN_INTC", 2), ("SR_DOMAIN_TIMER", 3),
                            ("SR_DOMAIN_THREADMAN", 4), ("SR_DOMAIN_IO", 5),
                            ("SR_DOMAIN_GE", 6), ("SR_DOMAIN_AUDIO", 7),
                            ("SR_DOMAIN_MEDIA", 8), ("SR_DOMAIN_COUNT", 9)):
            self.assertRegex(text, rf"{name}\s*=?\s*{value}\b")

    def test_header_defines_modes_with_hle_default_first(self):
        text = DOMAIN_H.read_text(encoding="utf-8")
        for name, value in (("SR_MODE_HLE", 0), ("SR_MODE_LLE", 1),
                            ("SR_MODE_COSIM", 2),
                            ("SR_MODE_LLE_FALLBACK_HLE", 3)):
            self.assertRegex(text, rf"{name}\s*=?\s*{value}\b")

    def test_header_declares_spec_apis(self):
        text = DOMAIN_H.read_text(encoding="utf-8")
        for proto in (
                r"sr_domain_mode_set\s*\(",
                r"sr_domain_mode_get\s*\(",
                r"sr_domain_reset_defaults\s*\(",
                r"sr_domain_for_library\s*\(",
                r"sr_domain_bind_nid\s*\(",
                r"sr_import_register_export\s*\(",
                r"sr_import_lookup_export\s*\(",
                r"sr_import_call\s*\("):
            self.assertRegex(text, proto)

    def test_hle_arm_is_exactly_the_existing_handler_path(self):
        text = DOMAIN_C.read_text(encoding="utf-8")
        self.assertIn("return (int)sr_syscall(s, nid);", text)

    def test_lle_arm_dispatches_and_fails_closed(self):
        text = DOMAIN_C.read_text(encoding="utf-8")
        self.assertIn("dispatch_call_try(s, guest_addr, stub_pc)", text)
        self.assertIn("SR_FLOW_FATAL", text)
        self.assertIn("SR_IMPORT_FATAL", text)

    def test_fallback_arm_logs_and_counts(self):
        text = DOMAIN_C.read_text(encoding="utf-8")
        self.assertIn("SR_IMPORT_FALLBACK_HLE", text)
        self.assertIn("s_fallback_count++", text)

    def test_cosim_arm_records_without_dual_execution(self):
        text = DOMAIN_C.read_text(encoding="utf-8")
        self.assertIn("s_cosim_count++", text)

    def test_library_table_covers_the_preflight_mapping(self):
        text = DOMAIN_C.read_text(encoding="utf-8")
        for library in ("ThreadManForUser", "ThreadManForKernel",
                        "IoFileMgrForUser", "IoFileMgrForKernel",
                        "sceGe_user", "sceAudio", "sceSasCore", "sceAtrac3plus",
                        "sceMpeg", "sceVideocodec", "InterruptManager",
                        "sceSysTimer", "SysTimerForKernel", "scePsmf"):
            self.assertIn(library, text)

    def test_recomp_header_shares_the_seam_declaration(self):
        text = (ROOT / "src" / "rt" / "recomp.h").read_text(encoding="utf-8")
        self.assertIn('#include "domain_mode.h"', text)


class MakefileWiringTests(unittest.TestCase):
    """The new selftest is wired like the existing native selftests."""

    def test_domain_mode_selftest_target_exists(self):
        self.assertIn("domain-mode-selftest:", MAKEFILE)
        recipe = MAKEFILE.split("domain-mode-selftest:", 1)[1].split("\n\n", 1)[0]
        self.assertIn("src/rt/domain_mode_selftest.c", recipe)
        self.assertIn("src/rt/guest_interp.c", recipe)
        self.assertIn("$(BUILD_DIR)/domain_mode_selftest.exe", recipe)

    def test_domain_mode_selftest_is_phony(self):
        phony = [line for line in MAKEFILE.splitlines() if line.startswith(".PHONY:")]
        self.assertTrue(any("domain-mode-selftest" in line for line in phony))

    def test_runtime_source_lists_carry_domain_mode(self):
        for var in ("RT_SRCS", "PORTABLE_CORE_SRCS"):
            block = re.search(rf"^{var}\s*:?=\s*(.*?)(?=\n\S|\Z)",
                              MAKEFILE, re.MULTILINE | re.DOTALL)
            self.assertIsNotNone(block, var)
            self.assertIn("src/rt/domain_mode.c", block.group(1))

    def test_every_guest_interp_link_also_links_domain_mode(self):
        offenders = []
        for number, raw in enumerate(MAKEFILE.splitlines(), start=1):
            if "src/rt/guest_interp.c" in raw and "-o " in raw:
                if "src/rt/domain_mode.c" not in raw:
                    offenders.append(f"Makefile:{number}: {raw.strip()}")
        self.assertEqual(offenders, [])

    def test_every_python_harness_link_also_links_domain_mode(self):
        # Same drift class as the cpu_lle audit in test_cpu_lle.py: tools/*.py
        # inline gcc commands that link guest_interp.c must also link
        # domain_mode.c. Only quoted path operands count, so prose mentions
        # in docstrings are not mistaken for link surfaces.
        offenders = []
        for path in sorted((ROOT / "tools").glob("*.py")):
            text = path.read_text(encoding="utf-8", errors="replace")
            if re.search(r"""["']guest_interp\.c["']""", text) and '"-o "' in text:
                if "domain_mode.c" not in text:
                    offenders.append(path.name)
        self.assertEqual(offenders, [])


if __name__ == "__main__":
    unittest.main()
