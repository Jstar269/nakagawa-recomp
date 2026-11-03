#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Generate the overlap-differential cases header for vfpu_interp_selftest.c.

Produces a header containing:
  - OVERLAP_DIFF_NCASES count
  - overlap_diff_cases[] table (instruction word + name)
  - overlap_diff_run_codegen() switch dispatcher

Every body is emitted by the production codegen path (tools/codegen.py
vfpu_effect), so the header stays in sync with the current emitter.
"""

from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import codegen


def _bin_word(op6: int, sub: int, size: int, vd: int, vs: int, vt: int) -> int:
    lo, hi = (size - 1) & 1, (size - 1) >> 1
    return (
        ((op6 & 0x3F) << 26)
        | ((sub & 7) << 23)
        | ((vt & 0x7F) << 16)
        | ((vs & 0x7F) << 8)
        | (vd & 0x7F)
        | (lo << 7)
        | (hi << 15)
    )


def _vv2_word(optype: int, size: int, vd: int, vs: int) -> int:
    """A VFPU4 unary-group word (vsin.s, vrexp2.s, ...): optype in bits 16..20."""
    lo, hi = (size - 1) & 1, (size - 1) >> 1
    return (
        (0x34 << 26)
        | ((optype & 0x1F) << 16)
        | ((vs & 0x7F) << 8)
        | (vd & 0x7F)
        | (lo << 7)
        | (hi << 15)
    )


def _matrix1_word(which: int, size: int, vd: int, vs: int) -> int:
    lo, hi = (size - 1) & 1, (size - 1) >> 1
    return (
        (0x3C << 26)
        | (28 << 21)
        | ((which & 0xF) << 16)
        | ((vs & 0x7F) << 8)
        | (vd & 0x7F)
        | (lo << 7)
        | (hi << 15)
    )


# One word per supported census row that had no differential evidence before this list
# (tools/vfpu_coverage_report.py credits a row when any of its words is in this set).
CENSUS_GAP_WORDS: tuple[int, ...] = (
    # major 0x1B: vcmp, vscmp at other vector registers/widths
    0x6C01810F, 0x6C01818F, 0x6C050101, 0x6C05010F, 0x6C058181, 0x6C0800FF,
    0x6C170181, 0x6C3F8101, 0x6E800000, 0x6E808001, 0x6E808090, 0x6E810190,
    # major 0x3C: matrix forms (vmmul/vtfm/vcrsp/vmmov/vmidt/vmzero/vmone/vrot...)
    0xF0000000, 0xF0800000, 0xF1000000, 0xF1800000, 0xF2000000, 0xF3800000,
    0xF3810000, 0xF3830000, 0xF3A0000F, 0xF3A00000, 0xF3A1008F, 0xF3A1800F,
    0xF3A181FF, 0xF3A30080, 0xF3A48000, 0xF3AA0080, 0xF3AB8181, 0xF3B0008F,
    0xF3B00000, 0xF3B0007F, 0xF3B0807F, 0xF3B100FF, 0xF3B18010, 0xF3B28090,
    0xF3B5808F, 0xF3B58000, 0xF3B68080, 0xF3B90080, 0xF3BB8101, 0xF3BF7FFF,
    0xF3BFFFFF,
)


def census_gap_size(word: int) -> int:
    """Vector size 1..4 from bit 7 (low) and bit 15 (high)."""
    return ((word >> 7) & 1 | ((word >> 15) & 1) << 1) + 1


def build_cases() -> tuple[list[tuple[int, str, int]], int, tuple[int, int]]:
    cases: list[tuple[int, str, int]] = []

    # vhdp.t (op 0x19 sub 4 size 3): destination overlaps sources
    cases.append((_bin_word(0x19, 4, 3, 0x20, 0x20, 0x00), "vhdp-t-partial-overlap", 5))

    # vhdp.t disjoint control
    cases.append((_bin_word(0x19, 4, 3, 0x10, 0x20, 0x00), "vhdp-t-disjoint", 5))

    # vhdp.t signed-zero corpus: keep the source vectors disjoint so the
    # first product can be -0. The explicit zero accumulator in the repaired
    # emitter then has an observable result under the host's IEEE add rules;
    # the historical chained expression does not.
    exotic_index = len(cases)
    cases.append((_bin_word(0x19, 4, 3, 0x08, 0x00, 0x04), "vhdp-t-signed-zero", 5))

    # The PSP-VFPU-CMP-001 probe words: vscmp.s / vsge.s / vslt.s S002, S000, S001
    # (VFPU3 sub 5/6/7, vd=0x40, vs=0x00, vt=0x20). check_vfpu_compare_measured_words()
    # finds them by name and pins both lanes to the 45 words a PSP-3000 returned.
    for sub, op in ((5, "vscmp"), (6, "vsge"), (7, "vslt")):
        cases.append((_bin_word(0x1B, sub, 1, 0x40, 0x00, 0x20), f"{op}-s-measured", 1))

    # The PSP-HLE-ISSUES-001 probe words (fixtures/psp_oracle/probe_hle_issues.c, issue #326):
    # vscmp.s/.p/.t/.q C200, C000, C100 and vscmp.q C200, C000, C000.
    # check_vscmp_hle_issues_cells() finds them by name and runs the probe's operand sets.
    for w, suffix, size in ((0x6E840008, "s", 1), (0x6E840088, "p", 2), (0x6E848008, "t", 3),
                            (0x6E848088, "q", 4), (0x6E808088, "q-same", 4)):
        cases.append((w, f"vscmp-{suffix}-issues", size))

    # The PSP-VFPU-TRANSCENDENTAL-001 probe word for vrexp2.s S002, S000 (optype 28).
    # check_vrexp2_measured_words() finds it by name and pins both lanes to the 63 words a
    # PSP-3000 returned.
    cases.append((_vv2_word(28, 1, 0x40, 0x00), "vrexp2-s-measured", 1))

    # vrndi.s (issue #326): the PSP-HLE-ISSUES-001 probe word vrndi.s S000, and the same draw
    # into S323 (vd=0x6E) so the destination decode is exercised off lane 0.
    # check_vrndi_rule_cells() finds them by name and steps both lanes from the probe's states.
    cases.append((0xD0210000, "vrndi-s-issues", 1))
    cases.append((0xD021006E, "vrndi-s-s323", 1))

    # PSP-VFPU-FORMS-001 (issue #326): the vfpu-forms probe words (probe_hle_issues.c).
    # check_vfpu_forms_measured() finds them by name and pins both lanes to the measured
    # words: vh2f.s C200, S000; vh2f.p C200, C000 and in place; vuc2i.s C200, S000;
    # vsocp.s/.p into C200; vwbn.s S200, S000 at the eight measured exponents; and
    # vrndf1.s / vrndf2.s S000.
    cases.append((0xD0330008, "h2f-s-forms", 1))
    cases.append((0xD0330088, "h2f-p-forms", 2))
    cases.append((0xD0330080, "h2f-p-inplace", 2))
    cases.append((0xD0380008, "uc2i-s-forms", 1))
    cases.append((0xD0450008, "socp-s-forms", 1))
    cases.append((0xD0450088, "socp-p-forms", 2))
    for e in (0x00, 0x01, 0x7E, 0x7F, 0x80, 0x96, 0xFE, 0xFF):
        cases.append((0xD3000008 | (e << 16), f"wbn-s-{e:02x}", 1))
    cases.append((0xD0220000, "rndf1-s-forms", 1))
    cases.append((0xD0230000, "rndf2-s-forms", 1))

    # vmscl.t (op 0x3C sub 4 size 3): scalar inside destination matrix
    cases.append((_bin_word(0x3C, 4, 3, 0x20, 0x20, 0x00), "vmscl-t-scalar-in-dest", 2))

    # vmscl.t disjoint control
    cases.append((_bin_word(0x3C, 4, 3, 0x10, 0x20, 0x00), "vmscl-t-disjoint", 2))

    # vmscl alias (Matrix1 which=1) with scalar inside destination
    cases.append((_matrix1_word(1, 3, 0x20, 0x20), "vmscl-alias-scalar-in-dest", 2))

    # vmscl alias disjoint control
    cases.append((_matrix1_word(1, 3, 0x10, 0x20), "vmscl-alias-disjoint", 2))

    # VFPU3 compare family (major 0x1B sub 5/6/7: vscmp, vsge, vslt) at every width
    # (single, pair, triple, quad), issue #326. Destination is matrix 0, sources
    # matrix 1 and 2, rows 0..n-1, so no lane is read and written by one word.
    # check_vfpu3_compare_parity() drives this range with its own operand corpus.
    cmp_first = len(cases)
    for sub, op in ((5, "vscmp"), (6, "vsge"), (7, "vslt")):
        for size, suffix in ((1, "s"), (2, "p"), (3, "t"), (4, "q")):
            cases.append((_bin_word(0x1B, sub, size, 0x00, 0x04, 0x08), f"{op}-{suffix}", size))
    cmp_range = (cmp_first, len(cases) - cmp_first)

    # Census-gap forms (issue #326): supported VFPU3 compare/min/max and VFPU6 matrix forms
    # that the synthetic fuzz corpus (minimal registers) does not reach. Each is one
    # representative word of a census row, taken from the census itself; check_overlap_diff()
    # runs the generated body and sr_vfpu_interp on the same state and compares every v[] lane
    # and vfpuCtrl[]. This is an AOT/interpreter cross-check, not PSP evidence.
    for w in CENSUS_GAP_WORDS:
        cases.append((w, f"census-{w:08x}", census_gap_size(w)))

    # Verify every word decodes through the production emitter without fallback.
    for w, name, _ in cases:
        body, _, _ = codegen.vfpu_effect(0x08900000, w)
        if "sr_vfpu_interp" in body:
            raise AssertionError(f"overlap diff word 0x{w:08x} ({name}) falls back to sr_vfpu_interp")

    return cases, exotic_index, cmp_range


def write_header(out_path: str, cases: list[tuple[int, str, int]], exotic_index: int,
                 cmp_range: tuple[int, int]) -> None:
    out = ["/* Generated by tools/vfpu_overlap_diff_gen.py. Do not edit. */", ""]
    out.append("#ifndef NAKAGAWA_VFPU_OVERLAP_DIFF_H")
    out.append("#define NAKAGAWA_VFPU_OVERLAP_DIFF_H")
    out.append("")
    out.append(f"#define OVERLAP_DIFF_NCASES {len(cases)}")
    out.append(f"#define OVERLAP_DIFF_VHDP_EXOTIC_INDEX {exotic_index}")
    out.append(f"#define OVERLAP_DIFF_CMP_FIRST {cmp_range[0]}")
    out.append(f"#define OVERLAP_DIFF_CMP_COUNT {cmp_range[1]}")
    out.append("")
    out.append("static const struct { uint32_t w; const char *name; } overlap_diff_cases[OVERLAP_DIFF_NCASES] = {")
    for w, name, _ in cases:
        out.append(f'    {{0x{w:08x}u, "{name}"}},')
    out.append("};")
    out.append("")
    # Emitted bodies are compiled with -w in generated chunks; some (matrix scalar forms) declare
    # a helper they do not read, which a -Werror strict compile of this header would reject.
    out.append("#pragma GCC diagnostic push")
    out.append('#pragma GCC diagnostic ignored "-Wunused-variable"')
    out.append("static void overlap_diff_run_codegen(CpuState *s, int idx) {")
    out.append("    switch (idx) {")
    for i, (w, name, _) in enumerate(cases):
        body, _, _ = codegen.vfpu_effect(0x08900000, w)
        out.append(f'    case {i}: /* {name} */ {body} break;')
    out.append("    default: break;")
    out.append("    }")
    out.append("}")
    out.append("#pragma GCC diagnostic pop")
    out.append("")
    out.append("#endif /* NAKAGAWA_VFPU_OVERLAP_DIFF_H */")
    Path(out_path).write_text("\n".join(out) + "\n", encoding="ascii", newline="\n")
    sys.stderr.write(f"vfpu_overlap_diff_gen: {len(cases)} cases -> {out_path}\n")


def main(argv: list[str] | None = None) -> int:
    if argv is None:
        argv = sys.argv
    if len(argv) < 2:
        sys.stderr.write("usage: vfpu_overlap_diff_gen.py <out.h>\n")
        return 2
    cases, exotic_index, cmp_range = build_cases()
    write_header(argv[1], cases, exotic_index, cmp_range)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
