# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the psp-recomp authors
"""Tests for tools/recomp_helper_inventory.py (issue #346).

The committed inventory is checked for stable facts only: names, defining files,
header declaration, reach class and provenance class. These tests pin that contract:
a line shift or an extra call site leaves the committed rows unchanged, while adding or
removing a helper, or changing a reach or provenance class, changes them.
"""

from __future__ import annotations

import contextlib
import copy
import io
import re
import tempfile
from pathlib import Path
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))

import recomp_helper_inventory as rhi  # noqa: E402


RECOMP_C_SAMPLE = """\
#include "recomp.h"
/* sr_commented(x) is prose, not a call */
static int s_count;
uint32_t sr_lwl(uint32_t rtv, uint32_t addr) {
    const char *t = "sr_in_literal(";
    return sr_r32(addr) + sr_bitrev(addr) + rtv;
}
static void sr_local(void) {
    sr_oor(0, 0, 0);
}
void sr_oor(uint32_t a, uint32_t v, int store) {
    (void)a; (void)v; (void)store;
}
static void *(*s_probe)(int) = sched_ext_probe;
"""

RECOMP_H_SAMPLE = """\
#ifndef PSP_RECOMP_RT_H
#define PSP_RECOMP_RT_H
#define MEM_R32(a) sr_r32(a)
#define MEM_LWL_PAIR(a, v) (sr_lwl((v), (a)))
static inline uint32_t sr_r32(uint32_t a) {
    return a;
}
#ifdef SR_EXTRA
static inline void sr_extra_only(void) {}
#endif
uint32_t sr_lwl(uint32_t rtv, uint32_t addr);
uint32_t sr_bitrev(uint32_t x);
void sr_oor(uint32_t a, uint32_t v, int store);
void (*sr_hook)(void);
typedef void (*RecompFn)(void *);
extern void *sched_ext_probe(int n);
uint32_t sr_selftest_probe(void);
SR_STATIC_ASSERT(1);
#endif
"""

SCHED_EXT_SAMPLE = """\
void *sched_ext_probe(int n) { return n ? (void *)0 : (void *)1; }
"""

# A harness stub: it defines sr_lwl and sr_selftest_probe so that a harness links, and
# it calls them. Neither definition is the runtime, and the calls are selftest-only.
STUB_SELFTEST_SAMPLE = """\
uint32_t sr_lwl(uint32_t rtv, uint32_t addr) { (void)rtv; return addr; }
uint32_t sr_selftest_probe(void) { return 7u; }
int probe_main(void) { return (int)(sr_lwl(1, 2) + sr_selftest_probe()); }
"""

EMITTER_SAMPLE = '''\
"""Emitter docstring mentions sr_doc_only(x)."""
def emit(addr):
    # sr_comment_only(x)
    a = f"sr_lwl({addr}, t);"
    b = "MEM_R32(" + str(addr) + ")"
    return a + b
'''

LEDGER_SAMPLE = {
    "entries": [
        {
            "path": "src/rt/recomp.c",
            "classification": "upstream_derived",
            "evidence": {"record_id": "cpu-core-runtime", "evidence_tier": "H"},
        },
        {
            "path": "src/rt/recomp.h",
            "classification": "upstream_derived",
            "evidence": {"record_id": "cpu-core-runtime", "evidence_tier": "H"},
        },
        {
            "path": "src/rt/sched_ext.c",
            "classification": "project_authored_attested",
            "evidence": {"record_id": "sched-ext", "evidence_tier": "S"},
        },
    ]
}

SAMPLE_HELPERS = {
    "sr_r32", "sr_extra_only", "sr_lwl", "sr_bitrev", "sr_local", "sr_oor",
    "sched_ext_probe", "sr_selftest_probe",
}

SAMPLE_REACH = {
    "sr_r32": "emitter-reached",
    "sr_lwl": "emitter-reached",
    "sr_bitrev": "production-runtime-only",
    "sr_oor": "production-runtime-only",
    "sr_local": "none",
    "sr_extra_only": "none",
    "sched_ext_probe": "address-only",
    "sr_selftest_probe": "selftest-only",
}


def _sources(recomp_c: str = RECOMP_C_SAMPLE, recomp_h: str = RECOMP_H_SAMPLE, **extra: str) -> dict[str, str]:
    sources = {
        rhi.RECOMP_C: recomp_c,
        rhi.RECOMP_H: recomp_h,
        "src/rt/sched_ext.c": SCHED_EXT_SAMPLE,
        "src/rt/stub_selftest.c": STUB_SELFTEST_SAMPLE,
    }
    sources.update(extra)
    return sources


def _build(sources=None, emitter=EMITTER_SAMPLE, ledger=None) -> dict:
    return rhi.build_inventory(
        sources if sources is not None else _sources(),
        emitter,
        ledger if ledger is not None else LEDGER_SAMPLE,
    )


def _render(**kwargs) -> str:
    return rhi.render_markdown(_build(**kwargs))


def _row(inv: dict, name: str) -> dict:
    return {r["name"]: r for r in inv["helpers"]}[name]


class StripTests(unittest.TestCase):
    def test_comments_and_literals_are_blank_and_lines_are_kept(self) -> None:
        src = 'a /* sr_x(1)\n */ b = "sr_y(";\n// sr_z(\nc;\n'
        out = rhi.strip_c(src)
        self.assertEqual(len(out), len(src))
        self.assertEqual(out.count("\n"), src.count("\n"))
        for hidden in ("sr_x", "sr_y", "sr_z"):
            self.assertNotIn(hidden, out)
        self.assertIn("c;", out)

    def test_directive_with_continuation_is_blank(self) -> None:
        src = "#define M(a) \\\n    sr_hidden(a)\nint keep;\n"
        out = rhi.strip_c(src)
        self.assertNotIn("sr_hidden", out)
        self.assertIn("int keep;", out)

    def test_char_literal_quote_does_not_open_a_string(self) -> None:
        out = rhi.strip_c("char q = '\"'; int after;")
        self.assertIn("int after;", out)

    def test_division_is_not_a_comment(self) -> None:
        out = rhi.strip_c("int q = a / b; sr_call(q);")
        self.assertIn("sr_call(q);", out)


class TopLevelDeclTests(unittest.TestCase):
    SNIPPET = """\
static inline uint32_t sr_r32(uint32_t a) {
    return a;
}
uint32_t sr_loaded_end(void);
typedef void (*RecompFn)(void *);
extern void (*sr_hook)(void);
struct Node { int x; void (*cb)(int); };
static const int table[] = { 1, 2 };
SR_STATIC_ASSERT(1);
_Static_assert(sizeof(int) == 4, "x");
__attribute__((noreturn)) void sr_fatal(void) { for (;;) { } }
int sr_mul(int a, int b) { return a * b; }
"""

    def test_definitions_and_prototypes_only(self) -> None:
        decls = {d.name: d for d in rhi.top_level_decls(rhi.strip_c(self.SNIPPET))}
        self.assertEqual(set(decls), {"sr_r32", "sr_loaded_end", "sr_fatal", "sr_mul"})
        self.assertTrue(decls["sr_r32"].is_def)
        self.assertTrue(decls["sr_r32"].static)
        self.assertTrue(decls["sr_r32"].inline)
        self.assertFalse(decls["sr_loaded_end"].is_def)
        self.assertFalse(decls["sr_mul"].static)

    def test_line_span_of_a_definition(self) -> None:
        text = self.SNIPPET
        starts = rhi._line_starts(text)
        decls = {d.name: d for d in rhi.top_level_decls(rhi.strip_c(text))}
        first = rhi._line(starts, decls["sr_r32"].name_offset)
        last = rhi._line(starts, decls["sr_r32"].end_offset)
        self.assertEqual((first, last), (1, 3))
        self.assertEqual(rhi._line(starts, decls["sr_mul"].name_offset), 12)


class MacroAndEmitterTests(unittest.TestCase):
    def test_macro_bodies_name_the_helpers_they_call(self) -> None:
        calls = rhi.macro_calls(RECOMP_H_SAMPLE)
        self.assertEqual(calls["MEM_R32"], {"sr_r32"})
        self.assertEqual(calls["MEM_LWL_PAIR"], {"sr_lwl"})

    def test_macro_continuation_and_comment_in_body(self) -> None:
        raw = "#define SR_X(a) do { \\\n    sr_inner(a); /* sr_prose(a) */ \\\n} while (0)\n"
        self.assertEqual(rhi.macro_calls(raw)["SR_X"], {"sr_inner"})

    def test_only_emitted_literal_text_counts(self) -> None:
        text = rhi.emitter_text(EMITTER_SAMPLE)
        self.assertIn("sr_lwl(", text)
        self.assertIn("MEM_R32(", text)
        self.assertNotIn("sr_doc_only", text)
        self.assertNotIn("sr_comment_only", text)


class InventoryTests(unittest.TestCase):
    def test_helper_set_excludes_prose_harnesses_and_non_functions(self) -> None:
        names = {r["name"] for r in _build()["helpers"]}
        self.assertEqual(names, SAMPLE_HELPERS)
        self.assertNotIn("sr_commented", names)
        self.assertNotIn("sr_in_literal", names)
        self.assertNotIn("RecompFn", names)
        self.assertNotIn("sr_hook", names)

    def test_reach_class_of_each_helper(self) -> None:
        inv = _build()
        self.assertEqual({r["name"]: r["reach"] for r in inv["helpers"]}, SAMPLE_REACH)

    def test_defined_in_and_header_declaration(self) -> None:
        inv = _build()
        self.assertEqual(_row(inv, "sr_lwl")["defined_in"], [rhi.RECOMP_C])
        self.assertEqual(_row(inv, "sr_bitrev")["defined_in"], [])
        self.assertTrue(_row(inv, "sr_bitrev")["in_header"])
        self.assertEqual(_row(inv, "sr_extra_only")["defined_in"], [rhi.RECOMP_H])
        self.assertEqual(_row(inv, "sched_ext_probe")["defined_in"], ["src/rt/sched_ext.c"])
        self.assertFalse(_row(inv, "sr_local")["in_header"])

    def test_harness_definition_is_not_a_defining_file(self) -> None:
        row = _row(_build(), "sr_lwl")
        self.assertEqual(row["defined_in"], [rhi.RECOMP_C])
        self.assertEqual(row["details"]["harness"], {"src/rt/stub_selftest.c": 1})

    def test_emitter_routes_direct_and_through_a_macro(self) -> None:
        inv = _build()
        self.assertTrue(_row(inv, "sr_lwl")["emitter_direct"])
        self.assertEqual(_row(inv, "sr_lwl")["emitter_macros"], [])
        self.assertFalse(_row(inv, "sr_r32")["emitter_direct"])
        self.assertEqual(_row(inv, "sr_r32")["emitter_macros"], ["MEM_R32"])
        self.assertEqual(_row(inv, "sr_r32")["details"]["emitter_macros"], {"MEM_R32": 1})

    def test_provenance_of_defining_files(self) -> None:
        inv = _build()
        self.assertEqual(
            _row(inv, "sched_ext_probe")["provenance"],
            [("src/rt/sched_ext.c", "project_authored_attested")],
        )
        self.assertEqual(_row(inv, "sr_r32")["home_class"], "upstream_derived")
        self.assertEqual(_row(inv, "sr_bitrev")["home_class"], rhi.DECLARED_ONLY)
        self.assertEqual(_row(inv, "sr_bitrev")["provenance"], [])

    def test_summary_counts_per_reach_and_provenance_class(self) -> None:
        s = _build()["summary"]
        self.assertEqual(s["total"], 8)
        self.assertEqual(s["in_header"], 7)
        self.assertEqual(
            s["reach"],
            {
                "emitter-reached": 2,
                "production-runtime-only": 2,
                "selftest-only": 1,
                "address-only": 1,
                "none": 2,
            },
        )
        self.assertEqual(sum(s["reach"].values()), s["total"])
        self.assertEqual(
            s["provenance"],
            {"declared-only": 2, "project_authored_attested": 1, "upstream_derived": 5},
        )

    def test_committed_rows_carry_no_line_numbers_or_call_counts(self) -> None:
        text = _render()
        self.assertIsNone(re.search(r"\bL\d", text))
        self.assertIsNone(re.search(r"\b\d+ in \d+ file", text))
        self.assertIsNone(re.search(r"\(direct \d", text))

    def test_details_carry_lines_and_counts_and_are_not_committed(self) -> None:
        inv = _build()
        details = rhi.render_details(inv)
        self.assertIsNotNone(re.search(r"`rt/recomp\.c` L\d+", details))
        self.assertIsNotNone(re.search(r"\| `sr_lwl` \|", details))
        self.assertNotIn("`rt/recomp.c` L", rhi.render_markdown(inv))

    def test_render_is_deterministic(self) -> None:
        self.assertEqual(_render(), _render())


class DriftContractTests(unittest.TestCase):
    """Only facts that matter for #346 may make the committed file stale."""

    def setUp(self) -> None:
        self.committed = _render()

    def test_line_shift_in_recomp_c_keeps_the_committed_rows(self) -> None:
        shifted = _sources(recomp_c="\n\n" + RECOMP_C_SAMPLE)
        self.assertEqual(_render(sources=shifted), self.committed)

    def test_line_shift_in_recomp_h_keeps_the_committed_rows(self) -> None:
        shifted = _sources(recomp_h="\n" + RECOMP_H_SAMPLE)
        self.assertEqual(_render(sources=shifted), self.committed)

    def test_extra_call_site_keeps_the_committed_rows(self) -> None:
        more = EMITTER_SAMPLE + '\nextra = f"sr_lwl({0}, t);"\n'
        self.assertEqual(_render(emitter=more), self.committed)
        more_runtime = _sources(
            recomp_c=RECOMP_C_SAMPLE.replace("(void)a; (void)v; (void)store;", "(void)a; (void)v; (void)store; (void)sr_bitrev(a);")
        )
        self.assertEqual(_render(sources=more_runtime), self.committed)

    def test_adding_a_helper_changes_the_committed_rows(self) -> None:
        added = RECOMP_C_SAMPLE + "static int sr_probe_added(int x) { return x + 1; }\n"
        changed = _render(sources=_sources(recomp_c=added))
        self.assertNotEqual(changed, self.committed)
        self.assertIn("`sr_probe_added`", changed)
        self.assertEqual(_build(sources=_sources(recomp_c=added))["summary"]["total"], 9)

    def test_emitter_reached_to_unreached_changes_the_committed_rows(self) -> None:
        # sr_lwl loses its only emitter call; its harness call makes it selftest-only.
        mutated = EMITTER_SAMPLE.replace('f"sr_lwl({addr}, t);"', 'f"sr_other({addr}, t);"')
        inv = _build(emitter=mutated)
        self.assertEqual(_row(inv, "sr_lwl")["reach"], "selftest-only")
        self.assertNotEqual(rhi.render_markdown(inv), self.committed)

    def test_emitter_route_removed_falls_back_to_runtime_and_changes_the_committed_rows(self) -> None:
        # sr_r32 keeps a production call site in recomp.c, so it is production-runtime-only.
        mutated = EMITTER_SAMPLE.replace('"MEM_R32(" + str(addr) + ")"', '"0"')
        inv = _build(emitter=mutated)
        self.assertEqual(_row(inv, "sr_r32")["reach"], "production-runtime-only")
        self.assertNotEqual(rhi.render_markdown(inv), self.committed)

    def test_removing_the_macro_changes_the_committed_rows(self) -> None:
        mutated = RECOMP_H_SAMPLE.replace("#define MEM_R32(a) sr_r32(a)\n", "")
        inv = _build(sources=_sources(recomp_h=mutated))
        self.assertEqual(_row(inv, "sr_r32")["emitter_macros"], [])
        self.assertNotEqual(rhi.render_markdown(inv), self.committed)

    def test_route_change_from_macro_to_direct_changes_the_committed_rows(self) -> None:
        mutated = EMITTER_SAMPLE.replace('"MEM_R32(" + str(addr) + ")"', '"sr_r32(" + str(addr) + ")"')
        inv = _build(emitter=mutated)
        self.assertTrue(_row(inv, "sr_r32")["emitter_direct"])
        self.assertNotEqual(rhi.render_markdown(inv), self.committed)

    def test_provenance_class_change_changes_the_committed_rows(self) -> None:
        ledger = copy.deepcopy(LEDGER_SAMPLE)
        ledger["entries"][2]["classification"] = "upstream_derived"
        self.assertNotEqual(_render(ledger=ledger), self.committed)

    def test_second_defining_file_changes_the_committed_rows(self) -> None:
        added = _sources(**{"src/rt/sched_ext_two.c": "void *sched_ext_probe(int n) { return 0; }\n"})
        inv = _build(sources=added)
        self.assertEqual(
            _row(inv, "sched_ext_probe")["defined_in"],
            ["src/rt/sched_ext.c", "src/rt/sched_ext_two.c"],
        )
        self.assertNotEqual(rhi.render_markdown(inv), self.committed)


class RepositoryInventoryTests(unittest.TestCase):
    """Checks against the real sources. Names and classes only, never line numbers."""

    @classmethod
    def setUpClass(cls) -> None:
        cls.inv = rhi.build_inventory(*rhi.load_inputs())
        cls.text = rhi.render_markdown(cls.inv)

    def test_committed_inventory_is_current(self) -> None:
        self.assertTrue(rhi.OUT_MD.is_file(), "run python tools/recomp_helper_inventory.py")
        self.assertEqual(
            rhi.OUT_MD.read_text(encoding="utf-8"),
            self.text,
            "docs/RECOMP_HELPER_INVENTORY.md is stale; run python tools/recomp_helper_inventory.py",
        )

    def test_committed_inventory_has_no_volatile_facts(self) -> None:
        text = rhi.OUT_MD.read_text(encoding="utf-8")
        self.assertIsNone(re.search(r"\bL\d", text))
        self.assertIsNone(re.search(r"\b\d+ in \d+ file", text))
        self.assertIsNone(re.search(r"\(direct \d", text))

    def test_known_repository_facts(self) -> None:
        by_name = {r["name"]: r for r in self.inv["helpers"]}
        self.assertEqual(by_name["sr_r32"]["reach"], "emitter-reached")
        self.assertEqual(by_name["sr_r32"]["emitter_macros"], ["MEM_R32"])
        self.assertTrue(by_name["sr_r32"]["in_header"])
        self.assertEqual(by_name["sr_lwl"]["reach"], "emitter-reached")
        self.assertTrue(by_name["sr_lwl"]["emitter_direct"])
        self.assertEqual(by_name["sched_init"]["defined_in"], ["src/rt/sched.c"])
        self.assertEqual(by_name["sched_init"]["reach"], "production-runtime-only")
        self.assertEqual(by_name["SDL_GetTicksNS"]["defined_in"], [])
        self.assertEqual(by_name["SDL_GetTicksNS"]["home_class"], rhi.DECLARED_ONLY)
        self.assertEqual(by_name["sr_nan_trap_dump_context"]["reach"], "address-only")
        self.assertEqual(by_name["sr_register_count"]["reach"], "none")
        self.assertEqual(sum(self.inv["summary"]["reach"].values()), self.inv["summary"]["total"])

    def test_check_mode_fails_on_a_stale_or_missing_file(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            stale = Path(tmp) / "stale.md"
            stale.write_text("not the inventory\n", encoding="utf-8")
            self.assertEqual(rhi.main(["--check", "--out", str(stale)]), 1)
            self.assertEqual(rhi.main(["--check", "--out", str(Path(tmp) / "absent.md")]), 1)

    def test_details_mode_prints_and_writes_nothing(self) -> None:
        before = rhi.OUT_MD.read_bytes()
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            self.assertEqual(rhi.main(["--details"]), 0)
        self.assertIn("| helper | defined at (lines) |", buf.getvalue())
        self.assertEqual(rhi.OUT_MD.read_bytes(), before)

    def test_details_cannot_be_combined_with_check(self) -> None:
        with self.assertRaises(SystemExit):
            with contextlib.redirect_stderr(io.StringIO()):
                rhi.main(["--check", "--details"])

    def test_harness_classification(self) -> None:
        self.assertTrue(rhi._is_harness("src/rt/hle_thread_selftest.c"))
        self.assertFalse(rhi._is_harness(rhi.RECOMP_C))


if __name__ == "__main__":
    unittest.main()
