#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""VFPU instruction coverage report for Nakagawa Recomp.

Categorizes VFPU/COP2 instruction coverage by family and documents the status
of each category: static emitter (codegen), interpreter (sr_vfpu_interp),
differential test (vfpu_fuzz.c), direct unit test, fallback-only, or untested.

This report is honest about what the historical "446/446 compute/prefix" claim means:
  - The 446-word corpus (previously tools/vfpu_words.txt, now gitignored as
    game-derived) covers only the compute/prefix opcode families 0x18/0x19/0x1B/
    0x34/0x37/0x3C that appear in the private game ELF.
  - It does NOT cover all VFPU instructions or all COP2 operations.
  - The public synthetic corpus has a 2742-word arithmetic/prefix/matrix
    baseline plus a separate 544-word memory/COP2 corpus.  The latter covers
    only the native-capable aligned memory and register-transfer forms; branch
    control flow and unaligned left/right forms remain explicit gaps.

Usage:
  python tools/vfpu_coverage_report.py [--format {text,json}]
"""

from __future__ import annotations

import argparse
import json
import sys

# ---------------------------------------------------------------------------
# Coverage matrix
# ---------------------------------------------------------------------------

# Each entry: (category, subcategory, has_emitter, has_interp, has_diff_test,
#              has_unit_test, notes)
COVERAGE_MATRIX = [
    # --- Compute / arithmetic ---
    (
        "compute", "binary arithmetic (vadd/vsub/vmul/vdiv)", True, True, True, False,
        "Covered by synthetic fuzzer (opcode 0x18). Both emitter and interpreter "
        "share sr_vread/sr_vwrite and prefix helpers -- zero divergence proves "
        "emitter/interp agreement, not hardware match."
    ),
    (
        "compute", "unary transforms (vmov/vabs/vneg/vsqrt/vrcp/vrsq)", True, True, True, False,
        "Covered by synthetic fuzzer (opcode 0x34 unary forms). Transcendental results "
        "(vrcp, vrsq) use shared math kernel -- tolerance-based comparison needed "
        "for PSP approximation semantics."
    ),
    (
        "compute", "min/max/sgn/compare (vmin/vmax/vsgn/vcmp)", True, True, True, False,
        "Covered by synthetic fuzzer (opcode 0x1B)."
    ),
    (
        "compute", "vector/matrix ops (vdot/vscl/vhdp/vfad/vavg)", True, True, True, False,
        "Covered by synthetic fuzzer (opcode 0x18 sub-ops). Dot-product "
        "accumulation order may differ from hardware for large inputs."
    ),
    # --- Prefix / state ---
    (
        "prefix/state", "VPFXS / VPFXT (source prefix)", True, True, True, False,
        "Source prefix decoding (swizzle, abs, neg, constant) shared between "
        "emitter and interpreter via the same prefix helper. Synthetic corpus "
        "exercises all swizzle permutations and modifier combinations. "
        "Independence: zero divergence proves agreement between emitter and "
        "interpreter; does NOT prove correctness vs. hardware."
    ),
    (
        "prefix/state", "VPFXD (destination prefix: mask + saturation)", True, True, True, False,
        "Covers all 16 destination mask combos and all 3 saturation modes "
        "(none / [0,1] / [-1,1]) in the synthetic corpus. Shared helper."
    ),
    (
        "prefix/state", "vcst (VFPU constant broadcast)", True, True, True, False,
        "Covered by the synthetic fuzzer (opcode 0x34 jump 3); the constant "
        "table is shared, so this is emitter/interpreter agreement rather than hardware evidence."
    ),
    (
        "prefix/state", "vflush / vnop control forms", True, False, False, False,
        "Emitter-owned control/no-op handling is present, but sr_vfpu_interp does "
        "not claim a matching oracle and the synthetic differential corpus does not include it."
    ),
    # --- Conversions ---
    (
        "conversion", "viim (integer immediate to float)", True, True, True, False,
        "Immediate encoding tested via synthetic corpus (word top byte 0xDF, "
        "opcode field 0x37). "
        "Boundary values 0, 1, 127, 128, 255 covered."
    ),
    (
        "conversion", "vfim (half-float immediate to float)", True, True, True, False,
        "Immediate encoding tested via synthetic corpus (word top byte 0xDF, "
        "opcode field 0x37). "
        "Same boundary values as viim."
    ),
    (
        "conversion", "vs2i / vi2uc / vi2c / vi2us / vi2s / vi2f / vf2i", True, True, True, False,
        "The supported conversion subset is emitted and compared by the "
        "synthetic corpus. Unsupported conversion forms are not included in this row."
    ),
    (
        "conversion", "vt2d / vuc2i / vf2h / vh2f / vrnds-family", False, True, False, False,
        "OPEN FALLBACK GAP: sr_vfpu_interp owns these forms, but no independent "
        "emitter/differential path is claimed."
    ),
    # --- Matrix / vector structured ops ---
    (
        "matrix/vector", "vmmul (matrix multiply)", True, True, True, False,
        "Covered by synthetic fuzzer (opcode 0x34). Matrix register-index "
        "mapping (transpose) exercised by non-zero matrix slot indices."
    ),
    (
        "matrix/vector", "vtfm / vhtfm (vector transform)", True, True, True, False,
        "Covered by synthetic fuzzer (opcode 0x34 sub-ops)."
    ),
    (
        "matrix/vector", "vqmul (quaternion multiply)", True, True, True, False,
        "Covered by synthetic fuzzer (opcode 0x34)."
    ),
    (
        "matrix/vector", "vdet (2D determinant)", True, True, True, False,
        "Covered by synthetic fuzzer (opcode 0x3C)."
    ),
    # --- COP2 moves ---
    (
        "COP2 moves", "mfc2 / mtc2 (VFPU scalar to/from GPR)", True, True, True, False,
        "Covered by the separate synthetic memory/COP2 corpus, including the "
        "physical scalar-register mapping and GPR side effects."
    ),
    (
        "COP2 moves", "cfc2 / ctc2 (VFPU control register I/O)", True, True, True, False,
        "Covered by the same corpus for control indices 0..15. Invalid control "
        "indices fail closed in both the emitter and oracle."
    ),
    # --- COP2 branches ---
    (
        "COP2 branch/control", "bvf / bvt / bvfl / bvtl (VFPU condition branch)", False, True, False, False,
        "FALLBACK ONLY: branch on VFPU condition code. No differential test -- "
        "branch semantics require control-flow comparison, not state comparison."
    ),
    # --- VFPU memory ---
    (
        "aligned memory", "lv.s / sv.s (VFPU 32-bit scalar load/store)", True, True, True, False,
        "Covered by the separate synthetic memory/COP2 corpus with an in-range "
        "harness-owned scratch page and full guest-memory comparison."
    ),
    (
        "aligned memory", "lv.q / sv.q (VFPU 128-bit quad load/store)", True, True, True, False,
        "The runtime emitter keeps a guarded fallback for dynamic alignment/span "
        "failures; the synthetic differential cases force a valid 16-byte-aligned "
        "span and compare the native path."
    ),
    (
        "unaligned left/right memory", "lvl.q / lvr.q / svl.q / svr.q", False, True, False, True,
        "OPEN DIFFERENTIAL GAP: direct interpreter/dispatch checks cover selected "
        "byte-lane behavior, but no independent emitter or control-flow/memory "
        "differential claim is made."
    ),
]


# ---------------------------------------------------------------------------
# Self-comparison check
# ---------------------------------------------------------------------------

def check_no_self_compare() -> list[str]:
    """Verify no differential test category compares interp vs interp.

    Returns a list of violations (should be empty for a correct corpus).
    """
    violations = []
    for row in COVERAGE_MATRIX:
        cat, sub, has_emitter, has_interp, has_diff_test, _, _ = row
        if has_diff_test and not has_emitter:
            violations.append(
                f"{cat}/{sub}: diff_test=True but has_emitter=False "
                "-- this would compare sr_vfpu_interp against itself"
            )
    return violations


# ---------------------------------------------------------------------------
# Report generation
# ---------------------------------------------------------------------------

def text_report() -> str:
    lines = [
        "VFPU Instruction Coverage Report — Nakagawa Recomp",
        "=" * 60,
        "",
        "IMPORTANT QUALIFICATIONS",
        "  The 446-word corpus formerly in tools/vfpu_words.txt was GAME-DERIVED",
        "  (extracted from the private eboot.elf). It is now git-ignored.",
        "  The public synthetic corpus (tools/vfpu_synth_gen.py) generates",
        "  words from public encoding knowledge — no game ELF required.",
        "",
        "  'compute/prefix 446/446' is a subset coverage claim:",
        "  it applies only to opcode families 0x18/0x19/0x1B/0x34/0x37/0x3C",
        "  and does NOT mean 'all VFPU instructions' or 'all COP2 operations'.",
        "  The public differential extension adds 544 words for native-capable",
        "  aligned memory and COP2 register-transfer forms; branch and unaligned",
        "  left/right forms remain explicitly open.",
        "",
        "DIFFERENTIAL INDEPENDENCE NOTE",
        "  The fuzzer compares codegen.vfpu_effect C vs sr_vfpu_interp.",
        "  Both share: sr_vread/sr_vwrite, prefix helpers, transcendental kernels.",
        "  Zero divergence proves: emitter and interpreter agree.",
        "  It does NOT prove: shared helpers match PSP hardware.",
        "",
    ]

    # Self-compare check
    violations = check_no_self_compare()
    if violations:
        lines.append("SELF-COMPARE VIOLATIONS (must be empty):")
        for v in violations:
            lines.append(f"  ERROR: {v}")
        lines.append("")
    else:
        lines.append("Self-compare check: OK (no diff test runs interp vs interp)")
        lines.append("")

    # Coverage matrix
    current_cat = None
    for row in COVERAGE_MATRIX:
        cat, sub, emitter, interp, diff, unit, notes = row
        if cat != current_cat:
            lines.append(f"\n[{cat.upper()}]")
            current_cat = cat
        status = []
        if emitter:
            status.append("emitter")
        if interp:
            status.append("interp")
        if diff:
            status.append("diff-test")
        if unit:
            status.append("unit-test")
        if not status:
            status_str = "UNTESTED"
        elif not emitter and not diff:
            status_str = "fallback-only (" + ", ".join(status) + ")"
        else:
            status_str = "covered (" + ", ".join(status) + ")"
        lines.append(f"  {sub}: {status_str}")
        # Wrap notes
        import textwrap
        for note_line in textwrap.wrap("Note: " + notes, width=72, subsequent_indent="        "):
            lines.append("    " + note_line)

    lines.append("")
    return "\n".join(lines)


def json_report() -> str:
    violations = check_no_self_compare()
    data = {
        "self_compare_violations": violations,
        "coverage": [
            {
                "category": row[0],
                "subcategory": row[1],
                "has_static_emitter": row[2],
                "has_interpreter": row[3],
                "has_differential_test": row[4],
                "has_unit_test": row[5],
                "notes": row[6],
            }
            for row in COVERAGE_MATRIX
        ],
    }
    return json.dumps(data, indent=2)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--format", choices=["text", "json"], default="text")
    args = parser.parse_args(argv)

    if args.format == "json":
        print(json_report())
    else:
        print(text_report())

    violations = check_no_self_compare()
    if violations:
        sys.stderr.write(f"ERROR: {len(violations)} self-compare violation(s) found\n")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
