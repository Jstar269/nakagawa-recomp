#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2025-2026 the psp-recomp authors

"""VFPU instruction coverage report for Nakagawa Recomp.

Categorizes VFPU/COP2 instruction coverage by family and documents the status
of each category: static emitter (codegen), interpreter (sr_vfpu_interp),
differential test (vfpu_fuzz.c), direct unit test, fallback-only, or untested.

This report is honest about what "446/446 compute/prefix" means:
  - The 446-word corpus (previously tools/vfpu_words.txt, now gitignored as
    game-derived) covers only the compute/prefix opcode families 0x18/0x19/0x1B/
    0x34/0x37/0x3C that appear in the private game ELF.
  - It does NOT cover all VFPU instructions or all COP2 operations.
  - The synthetic corpus (tools/vfpu_synth_gen.py) generates words in the same
    opcode families without the private ELF.

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
        "Covered by synthetic fuzzer (opcode 0x19). Transcendental results "
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
        "prefix/state", "VFPU control register (vcst / vnop / vflush)", False, False, False, False,
        "UNTESTED: no static emitter for vcst (constant source like pi/2); "
        "no differential test. Fallback to sr_vfpu_interp for any codegen miss."
    ),
    # --- Conversions ---
    (
        "conversion", "viim (integer immediate to float)", True, True, True, False,
        "Immediate encoding tested via synthetic corpus (opcode 0xDF). "
        "Boundary values 0, 1, 127, 128, 255 covered."
    ),
    (
        "conversion", "vfim (half-float immediate to float)", True, True, True, False,
        "Immediate encoding tested via synthetic corpus (opcode 0x7F). "
        "Same boundary values as viim."
    ),
    (
        "conversion", "vt2d / vuc2i / vs2i / vi2f / vf2i / vf2h / vh2f", False, True, False, False,
        "FALLBACK ONLY: sr_vfpu_interp implements these; no codegen emitter for "
        "the full family. No differential test -- both sides would be interp."
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
        "COP2 moves", "mfc2 (VFPU scalar to GPR)", False, True, False, False,
        "FALLBACK ONLY: sr_vfpu_interp handles mfc2; no dedicated emitter path. "
        "No differential test without emitter."
    ),
    (
        "COP2 moves", "mtc2 (GPR to VFPU scalar)", False, True, False, False,
        "FALLBACK ONLY: same as mfc2."
    ),
    (
        "COP2 moves", "cfc2 / ctc2 (VFPU control register I/O)", False, True, False, False,
        "FALLBACK ONLY. VFPU control registers (VPFXS, VPFXT, VPFXD, CC) are "
        "accessible via cfc2/ctc2; only sr_vfpu_interp handles these paths."
    ),
    # --- COP2 branches ---
    (
        "COP2 branch/control", "bvf / bvt / bvfl / bvtl (VFPU condition branch)", False, True, False, False,
        "FALLBACK ONLY: branch on VFPU condition code. No differential test -- "
        "branch semantics require control-flow comparison, not state comparison."
    ),
    # --- VFPU memory ---
    (
        "aligned memory", "lv.s / sv.s (VFPU 32-bit scalar load/store)", False, True, False, False,
        "FALLBACK ONLY: no static emitter for VFPU memory ops; sr_vfpu_interp "
        "handles load/store. Memory address alignment is not tested differentially."
    ),
    (
        "aligned memory", "lv.q / sv.q (VFPU 128-bit quad load/store)", False, True, False, False,
        "FALLBACK ONLY: same as lv.s. 128-bit alignment requirement not unit-tested."
    ),
    (
        "unaligned left/right memory", "lvl.q / lvr.q / svl.q / svr.q", False, True, False, False,
        "FALLBACK ONLY: unaligned VFPU quad loads/stores. No emitter, no differential "
        "test. Alignment/byte-lane behavior not verified."
    ),
    (
        "stores", "sv.s / sv.q (stores -- same as aligned above)", False, True, False, False,
        "Grouped with aligned memory for clarity. Same fallback status."
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
