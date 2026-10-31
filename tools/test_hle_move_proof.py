# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for tools/hle_move_proof.py (issue #307).

A synthetic base ``hle.c`` and a correct split of it pass; each mutation of the split that
changes behaviour fails for its own reason. The preprocessed layers need a C compiler and
are skipped, visibly, without one.
"""

from __future__ import annotations

from pathlib import Path
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

import hle_move_proof as proof  # noqa: E402

BASE_HLE = """\
#include <stdint.h>
typedef struct CpuState { uint32_t r[32]; } CpuState;
typedef uint32_t (*HleFn)(CpuState *s);
void sr_hle_register(uint32_t nid, const char *name, HleFn fn) { (void)nid; (void)name; (void)fn; }
#define A0 (s->r[4])
/* shared between two subsystems */
static int s_shared = 0;
static uint32_t shared_helper(uint32_t x) { return x + (uint32_t)s_shared; }
static uint32_t h_AudioOut(CpuState *s) { return shared_helper(A0); }
static uint32_t h_IoOpen(CpuState *s) { return shared_helper(A0) + 1u; }
static uint32_t arg1(CpuState *s) { return s->r[5]; }
static uint32_t h_AudioLen(CpuState *s) { return arg1(s); }
static uint32_t h_IoLen(CpuState *s) { return arg1(s); }
#ifdef SR_HLE_THREAD_SELFTEST
void sr_hle_test_hook(void) { s_shared = 1; }
#endif
static int s_registered;
static void hle_register_mixed_handlers(void) {
    if (s_registered) return;
    s_registered = 1;
    sr_hle_register(0x10u, "sceAudioOutput", h_AudioOut);
    sr_hle_register(0x20u, "sceIoOpen", h_IoOpen);
}
void sr_hle_init(void) {
    hle_register_mixed_handlers();
    sr_hle_register(0x30u, "sceAudioOther", h_AudioOut);
}
"""

SPLIT = {
    "src/rt/hle_internal.h": """\
#ifndef SR_HLE_INTERNAL_H
#define SR_HLE_INTERNAL_H
#include <stdint.h>
typedef struct CpuState { uint32_t r[32]; } CpuState;
typedef uint32_t (*HleFn)(CpuState *s);
#define A0 (s->r[4])
void sr_hle_register(uint32_t nid, const char *name, HleFn fn);
uint32_t shared_helper(uint32_t x);
static inline uint32_t arg1(CpuState *s) { return s->r[5]; }
void hle_register_mixed_audio(void);
void hle_register_mixed_io(void);
void hle_register_init_audio(void);
#endif
""",
    "src/rt/hle.c": """\
#include "hle_internal.h"
void sr_hle_register(uint32_t nid, const char *name, HleFn fn) { (void)nid; (void)name; (void)fn; }
/* shared between two subsystems */
static int s_shared = 0;
uint32_t shared_helper(uint32_t x) { return x + (uint32_t)s_shared; }
#ifdef SR_HLE_THREAD_SELFTEST
void sr_hle_test_hook(void) { s_shared = 1; }
#endif
static int s_registered;
static void hle_register_mixed_handlers(void) {
    if (s_registered) return;
    s_registered = 1;
    hle_register_mixed_audio();
    hle_register_mixed_io();
}
void sr_hle_init(void) {
    hle_register_mixed_handlers();
    hle_register_init_audio();
}
""",
    "src/rt/hle_audio.c": """\
#include "hle_internal.h"
static uint32_t h_AudioOut(CpuState *s) { return shared_helper(A0); }
static uint32_t h_AudioLen(CpuState *s) { return arg1(s); }
void hle_register_mixed_audio(void) {
    sr_hle_register(0x10u, "sceAudioOutput", h_AudioOut);
}
void hle_register_init_audio(void) {
    sr_hle_register(0x30u, "sceAudioOther", h_AudioOut);
}
""",
    "src/rt/hle_io.c": """\
#include "hle_internal.h"
static uint32_t h_IoOpen(CpuState *s) { return shared_helper(A0) + 1u; }
static uint32_t h_IoLen(CpuState *s) { return arg1(s); }
void hle_register_mixed_io(void) {
    sr_hle_register(0x20u, "sceIoOpen", h_IoOpen);
}
""",
}

BASE_UNITS = [("src/rt/hle.c", BASE_HLE)]
NEW_C = ("src/rt/hle.c", "src/rt/hle_audio.c", "src/rt/hle_io.c")
NEW_H = ("src/rt/hle_internal.h",)


def new_units(split: dict[str, str]) -> list[tuple[str, str]]:
    return [(path, split[path]) for path in NEW_C + NEW_H]


def mutate(path: str, old: str, new: str) -> dict[str, str]:
    split = dict(SPLIT)
    assert old in split[path], old
    split[path] = split[path].replace(old, new, 1)
    return split


class ItemLayerTests(unittest.TestCase):
    def test_correct_split_passes(self) -> None:
        report = proof.compare_items(BASE_UNITS, new_units(SPLIT))
        self.assertEqual(report.errors, [])
        self.assertEqual(sorted(report.static_removed),
                         ["shared_helper (src/rt/hle.c)"])
        self.assertEqual(sorted(report.regrouped), ["hle_register_mixed_handlers", "sr_hle_init"])
        self.assertEqual(len(report.new_glue), 3)
        self.assertEqual(report.made_inline, ["arg1 (src/rt/hle_internal.h)"])
        self.assertEqual(report.moved_to["h_IoOpen"], "src/rt/hle_io.c")
        self.assertEqual(report.missing_comments, [])

    def test_static_after_a_leading_attribute_may_be_dropped(self) -> None:
        attr = "__attribute__((format(printf, 1, 2)))"
        base = [("src/rt/hle.c", f"void sr_hle_init(void) {{}}\n{attr}\nstatic int note(const char *f, ...) {{ return 0; }}\n"
                                 "int use_a(void) { return note(\"a\"); }\n")]
        new = [("src/rt/hle.c", f"void sr_hle_init(void) {{}}\n{attr}\nint note(const char *f, ...) {{ return 0; }}\n"),
               ("src/rt/hle_a.c", '#include "hle_internal.h"\nint use_a(void) { return note("a"); }\n'),
               ("src/rt/hle_internal.h", f"{attr} int note(const char *f, ...);\n")]
        report = proof.compare_items(base, new)
        self.assertEqual(report.errors, [])
        self.assertEqual(report.static_removed, ["note (src/rt/hle.c)"])

    def test_static_dropped_from_a_declaration_whose_second_name_another_file_uses(self) -> None:
        base = [("src/rt/hle.c", "void sr_hle_init(void) {}\nstatic int s_a, s_b;\nint use_b(void) { return s_b; }\n")]
        new = [("src/rt/hle.c", "void sr_hle_init(void) {}\nint s_a, s_b;\n"),
               ("src/rt/hle_a.c", '#include "hle_internal.h"\nint use_b(void) { return s_b; }\n'),
               ("src/rt/hle_internal.h", "#pragma GCC visibility push(hidden)\nextern int s_a, s_b;\n"
                                         "#pragma GCC visibility pop\n")]
        report = proof.compare_items(base, new)
        self.assertEqual(report.errors, [])
        self.assertEqual(report.static_removed_names, {"s_a": ["s_a", "s_b"]})

    def assert_l1_error(self, split: dict[str, str], fragment: str) -> None:
        errors = proof.compare_items(BASE_UNITS, new_units(split)).errors
        self.assertTrue(any(fragment in e for e in errors), errors)

    def test_edited_message_fails(self) -> None:
        self.assert_l1_error(mutate("src/rt/hle_io.c", '"sceIoOpen"', '"sceIoOpen2"'), "string literals changed")

    def test_object_defined_twice_fails(self) -> None:
        split = mutate("src/rt/hle_io.c", '#include "hle_internal.h"\n',
                       '#include "hle_internal.h"\nstatic int s_shared = 0;\n')
        self.assert_l1_error(split, "appears 2 times")

    def test_function_moved_out_of_its_conditional_fails(self) -> None:
        split = mutate("src/rt/hle.c", "#ifdef SR_HLE_THREAD_SELFTEST\n", "")
        split["src/rt/hle.c"] = split["src/rt/hle.c"].replace("#endif\n", "", 1)
        self.assert_l1_error(split, "sr_hle_test_hook is under []")

    def test_changed_body_fails(self) -> None:
        self.assert_l1_error(mutate("src/rt/hle_io.c", "+ 1u;", "+ 2u;"), "h_IoOpen is missing or changed")

    def test_new_function_with_behaviour_fails(self) -> None:
        split = mutate("src/rt/hle_io.c", "    sr_hle_register(0x20u", "    s_shared = 2;\n    sr_hle_register(0x20u")
        self.assert_l1_error(split, "does more than register NIDs")

    def test_unneeded_static_removal_fails(self) -> None:
        split = mutate("src/rt/hle_io.c", "static uint32_t h_IoOpen", "uint32_t h_IoOpen")
        split["src/rt/hle_internal.h"] = split["src/rt/hle_internal.h"].replace(
            "#endif\n", "uint32_t h_IoOpen(CpuState *s);\n#endif\n")
        self.assert_l1_error(split, "which no other HLE file uses")

    def test_object_defined_in_the_header_fails(self) -> None:
        split = mutate("src/rt/hle.c", "static int s_shared = 0;\n", "")
        split["src/rt/hle_internal.h"] = split["src/rt/hle_internal.h"].replace(
            "#endif\n", "static int s_shared = 0;\n#endif\n")
        self.assert_l1_error(split, "header defines object s_shared")

    def test_address_of_a_header_inline_function_fails(self) -> None:
        split = mutate("src/rt/hle_io.c", "static uint32_t h_IoLen",
                       "static uint32_t (*s_len)(CpuState *) = arg1;\nstatic uint32_t h_IoLen")
        self.assert_l1_error(split, "the address of inline function arg1 is taken")

    def test_lost_comment_is_listed(self) -> None:
        split = mutate("src/rt/hle.c", "/* shared between two subsystems */\n", "")
        report = proof.compare_items(BASE_UNITS, new_units(split))
        self.assertEqual(report.missing_comments, ["/* shared between two subsystems */"])


class FlattenTests(unittest.TestCase):
    def definitions(self, **bodies: str) -> dict[str, list[str]]:
        return {name: proof._TOKEN.findall(f"void {name}(void) {{ {body} }}") for name, body in bodies.items()}

    def test_registration_functions_are_inlined_in_order(self) -> None:
        defs = self.definitions(
            sr_hle_init="a(); hook(1); b();",
            a='sr_hle_register(1u, "x", h);',
            b='sr_hle_register(2u, "y", h);',
        )
        flat = " ".join(proof.flatten_init(defs))
        self.assertEqual(flat, 'sr_hle_register ( 1u , "x" , h ) ; hook ( 1 ) ; sr_hle_register ( 2u , "y" , h ) ;')

    def test_every_entry_point_that_registers_is_a_root(self) -> None:
        defs = self.definitions(
            sr_hle_init="a(); b();",
            sr_hle_test_register_b="b();",
            a='sr_hle_register(1u, "x", h);',
            b='if (g) return; sr_hle_register(2u, "y", h);',
            unrelated="hook(1);",
        )
        self.assertEqual(proof.registration_roots(defs), ["sr_hle_init", "sr_hle_test_register_b"])

    def test_a_returning_body_is_scoped(self) -> None:
        defs = self.definitions(sr_hle_init="a();", a='if (g) return; sr_hle_register(1u, "x", h);')
        self.assertEqual(proof.flatten_init(defs)[0], "@{")
        self.assertEqual(proof.flatten_init(defs)[-1], "@}")


@unittest.skipIf(shutil.which("gcc") is None, "needs gcc for the preprocessed layers")
class PreprocessedLayerTests(unittest.TestCase):
    def run_layers(self, split: dict[str, str]) -> list[str]:
        report = proof.compare_items(BASE_UNITS, new_units(split))
        with tempfile.TemporaryDirectory() as tmp:
            base_tree, new_tree = Path(tmp, "base"), Path(tmp, "new")
            (base_tree / "src/rt").mkdir(parents=True)
            (base_tree / "src/rt/hle.c").write_text(BASE_HLE, encoding="utf-8")
            for path, text in split.items():
                (new_tree / path).parent.mkdir(parents=True, exist_ok=True)
                (new_tree / path).write_text(text, encoding="utf-8")
            configs = {k: proof.CONFIGS[k] for k in ("production", "thread-selftest")}
            proof.compare_preprocessed(base_tree, ["src/rt/hle.c"], (), new_tree, list(NEW_C), NEW_H,
                                       report, "gcc", configs)
        return report.errors

    def test_correct_split_passes(self) -> None:
        self.assertEqual(self.run_layers(SPLIT), [])

    def test_swapped_registration_order_fails(self) -> None:
        split = mutate("src/rt/hle.c", "    hle_register_mixed_audio();\n    hle_register_mixed_io();\n",
                       "    hle_register_mixed_io();\n    hle_register_mixed_audio();\n")
        errors = self.run_layers(split)
        self.assertTrue(any("flattened registration sequence differs" in e for e in errors), errors)

    def test_macro_that_expands_differently_fails(self) -> None:
        split = mutate("src/rt/hle_io.c", '#include "hle_internal.h"\n',
                       '#include "hle_internal.h"\n#undef A0\n#define A0 (s->r[5])\n')
        errors = self.run_layers(split)
        self.assertTrue(any("h_IoOpen preprocesses differently" in e for e in errors), errors)

    def test_missing_declaration_fails_even_where_the_compiler_only_warns(self) -> None:
        split = mutate("src/rt/hle_internal.h", "uint32_t shared_helper(uint32_t x);\n", "")
        errors = self.run_layers(split)
        self.assertTrue(any("hle_io.c does not compile with declarations enforced" in e for e in errors), errors)

    def test_selftest_hook_left_out_of_a_configuration_fails(self) -> None:
        split = mutate("src/rt/hle.c", "#ifdef SR_HLE_THREAD_SELFTEST\n", "#ifdef SR_SOMETHING_ELSE\n")
        errors = self.run_layers(split)
        self.assertTrue(any("[thread-selftest] sr_hle_test_hook is defined in the base" in e for e in errors), errors)


if __name__ == "__main__":
    unittest.main()
