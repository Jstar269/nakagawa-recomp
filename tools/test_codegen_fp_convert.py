# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Execute generated COP1 conversions against independent fixed vectors.

The owned ELF fixture contains real MIPS encodings for round/trunc/ceil/floor,
cvt.w.s and cvt.s.w.  The test invokes tools/codegen.py, compiles its emitted C,
and runs the translated function.  Linux keeps float-cast-overflow UBSan enabled
so generated/reference agreement cannot conceal the historical shared UB.
"""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
CODEGEN = ROOT / "tools" / "codegen.py"
CC = shutil.which("gcc")
ENTRY = 0x00001000

# Isolated translation-unit contract for generated output: the same interface the
# real runtime exposes, with accepting no-op stubs so an isolated harness does not
# link the whole runtime.
ISOLATED_RECOMP_H = """\
#ifndef TEST_RECOMP_H
#define TEST_RECOMP_H
#include <stdint.h>
#include "fp_convert.h"
typedef struct CpuState {
    uint32_t r[32];
    uint32_t hi, lo, pc;
    union { float f[32]; uint32_t fi[32]; };
    uint32_t fcr31, fpcond;
    union { float v[128]; uint32_t vi[128]; };
    uint32_t vfpuCtrl[16];
} CpuState;
#define SR_YIELD(s, pc) ((void)0)
#define sr_begin(s, pc, op) ((void)0)
#define sr_end(s, addr, size) ((void)0)
void dispatch(CpuState *s, uint32_t target);
void sr_register(uint32_t addr, void (*fn)(CpuState *));
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc);
void sr_hle_call(CpuState *s, uint32_t nid);
void sr_syscall(CpuState *s, uint32_t nid);
void sr_unimplemented(uint32_t addr, const char *reason);
void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix);
void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t prefix);
/* Generated sr_register_all() opens with the exec-span registry contract
 * (sr_exec_span_reset + one sr_exec_span_register per executable span,
 * failing closed when registration returns zero). This isolated harness
 * declares the same generated interface and supplies accepting no-op
 * stubs below instead of linking the whole runtime. */
void sr_exec_span_reset(void);
int sr_exec_span_register(uint32_t start, uint32_t end);
#endif
"""

ISOLATED_STUBS_C = """\
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "recomp.h"

void sr_register(uint32_t addr, void (*fn)(CpuState *)) { (void)addr; (void)fn; }
void sr_exec_span_reset(void) {}
int sr_exec_span_register(uint32_t start, uint32_t end) { (void)start; (void)end; return 1; }
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) { (void)s; (void)code; (void)pc; }
void sr_hle_call(CpuState *s, uint32_t nid) { (void)s; (void)nid; }
void sr_syscall(CpuState *s, uint32_t nid) { (void)s; (void)nid; }
void sr_unimplemented(uint32_t addr, const char *reason) { (void)addr; (void)reason; abort(); }
void dispatch(CpuState *s, uint32_t target) { (void)s; (void)target; abort(); }
void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix) {
    (void)prefix;
    for (int i = 0; i < n; i++) memcpy(&r[i], &s->vi[idx[i]], 4);
}
void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t prefix) {
    (void)prefix;
    for (int i = 0; i < n; i++) memcpy(&s->vi[idx[i]], &d[i], 4);
}
"""



def _synthetic_elf(words: list[int]) -> bytes:
    """Build a minimal public ELF32/MIPS executable containing only .text."""
    text = b"".join(struct.pack("<I", word & 0xFFFFFFFF) for word in words)
    data_off = 0x100
    shstr = b"\0.text\0.shstrtab\0"
    shstr_off = data_off + len(text)
    shoff = (shstr_off + len(shstr) + 3) & ~3
    blob = bytearray(shoff + 3 * 40)
    ident = b"\x7fELF" + bytes([1, 1, 1, 0]) + b"\0" * 8
    blob[:52] = ident + struct.pack(
        "<HHIIIIIHHHHHH",
        2, 8, 1, ENTRY, 52, shoff, 0x50001000,
        52, 32, 1, 40, 3, 2,
    )
    blob[52:84] = struct.pack(
        "<8I", 1, data_off, ENTRY, ENTRY, len(text), len(text), 5, 0x1000
    )
    blob[data_off:data_off + len(text)] = text
    blob[shstr_off:shstr_off + len(shstr)] = shstr
    sections = [
        (0, 0, 0, 0, 0, 0, 0, 0, 0, 0),
        (1, 1, 6, ENTRY, data_off, len(text), 0, 0, 4, 0),
        (7, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0),
    ]
    for index, section in enumerate(sections):
        struct.pack_into("<10I", blob, shoff + index * 40, *section)
    return bytes(blob)


def _fps(fs: int, fd: int, funct: int) -> int:
    return (0x11 << 26) | (0x10 << 21) | (fs << 11) | (fd << 6) | funct


def _mfc1(rt: int, fs: int) -> int:
    return (0x11 << 26) | (rt << 16) | (fs << 11)


def _fpw(fs: int, fd: int) -> int:
    return (0x11 << 26) | (0x14 << 21) | (fs << 11) | (fd << 6) | 0x20


def _fps3(ft: int, fs: int, fd: int, funct: int) -> int:
    """Three-operand COP1 S-format arithmetic (add/sub/mul/div.s)."""
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (fd << 6) | funct


def _ctc1(rt: int) -> int:
    return (0x11 << 26) | (0x06 << 21) | (rt << 16) | (31 << 11)


def _cfc1(rt: int) -> int:
    return (0x11 << 26) | (0x02 << 21) | (rt << 16) | (31 << 11)


def _ccond(ft: int, fs: int, cond: int) -> int:
    """c.cond.s with cc=0 (FCC0)."""
    return (0x11 << 26) | (0x10 << 21) | (ft << 16) | (fs << 11) | (0 << 8) | (0x30 | cond)


def _ori(rt: int, imm: int) -> int:
    return (0x0D << 26) | (0 << 21) | (rt << 16) | (imm & 0xFFFF)


def _lui(rt: int, imm: int) -> int:
    return (0x0F << 26) | (rt << 16) | (imm & 0xFFFF)


def _bc1(tf: int, offset_words: int) -> int:
    """bc1f/bc1t with a word offset relative to the delay slot."""
    return (0x11 << 26) | (8 << 21) | ((tf & 1) << 16) | (offset_words & 0xFFFF)



def _vf2i(mode: int, scale: int, vd: int) -> int:
    return (
        (0x34 << 26) | ((16 + mode) << 21) | (scale << 16) | vd
    )


def _fixture_words() -> list[int]:
    words: list[int] = []
    for fd, rt, funct in (
        (1, 8, 0x0C),   # round.w.s
        (2, 9, 0x0D),   # trunc.w.s
        (3, 10, 0x0E),  # ceil.w.s
        (4, 11, 0x0F),  # floor.w.s
        (5, 12, 0x24),  # cvt.w.s (fcr31-directed)
    ):
        words.extend((_fps(0, fd, funct), _mfc1(rt, fd)))
    words.extend((_fpw(6, 7), _mfc1(13, 7)))  # signed-word reinterpretation
    words.extend((
        _vf2i(0, 0, 1),   # vf2in.s s001, s000 -> physical vi[4]
        _vf2i(1, 0, 2),   # vf2iz.s s002, s000 -> physical vi[8]
        _vf2i(2, 0, 3),   # vf2iu.s s003, s000 -> physical vi[12]
        _vf2i(3, 0, 4),   # vf2id.s s010, s000 -> physical vi[16]
        _vf2i(0, 31, 5),  # scaled nearest extreme -> physical vi[20]
    ))
    words.extend((0x03E00008, 0x00000000))     # jr ra; nop
    return words


@unittest.skipUnless(CC, "gcc is required for the generated-C conversion regression")
class GeneratedFpConversionTests(unittest.TestCase):
    def test_generated_instructions_match_fixed_vectors_without_ub(self):
        assert CC is not None
        with tempfile.TemporaryDirectory(prefix="codegen_fp_convert_") as tmp:
            work = Path(tmp)
            elf = work / "fp_convert.elf"
            generated = work / "fp_convert.c"
            elf.write_bytes(_synthetic_elf(_fixture_words()))

            env = dict(os.environ)
            env["HST_EXTRA_SPANS"] = ""
            result = subprocess.run(
                [sys.executable, str(CODEGEN), str(elf), str(generated), "--profile=hst"],
                cwd=ROOT, env=env, capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

            emitted = "\n".join(
                path.read_text(encoding="ascii")
                for path in [generated, *sorted(work.glob("fp_convert_*.c"))]
            )
            self.assertIn("sr_fpu_to_word", emitted)
            self.assertIn("sr_vfpu_to_word", emitted)
            self.assertIn("s->fcr31", emitted)
            self.assertIn("sr_u32_as_s32", emitted)
            self.assertNotIn("sr_to_w", emitted)
            self.assertNotIn("nearbyint", emitted)

            (work / "recomp.h").write_text(ISOLATED_RECOMP_H, encoding="ascii")
            (work / "harness.c").write_text(
                """\
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fp_convert_funcs.h"

void sr_register(uint32_t addr, void (*fn)(CpuState *)) { (void)addr; (void)fn; }
void sr_exec_span_reset(void) {}
int sr_exec_span_register(uint32_t start, uint32_t end) { (void)start; (void)end; return 1; }
void sr_raw_syscall(CpuState *s, uint32_t code, uint32_t pc) { (void)s; (void)code; (void)pc; }
void sr_hle_call(CpuState *s, uint32_t nid) { (void)s; (void)nid; }
void sr_syscall(CpuState *s, uint32_t nid) { (void)s; (void)nid; }
void sr_unimplemented(uint32_t addr, const char *reason) { (void)addr; (void)reason; abort(); }
void dispatch(CpuState *s, uint32_t target) { (void)s; (void)target; abort(); }
void sr_vread(float *r, const CpuState *s, const uint8_t *idx, int n, uint32_t prefix) {
    (void)prefix;
    for (int i = 0; i < n; i++) memcpy(&r[i], &s->vi[idx[i]], 4);
}
void sr_vwrite(CpuState *s, const uint8_t *idx, float *d, int n, uint32_t prefix) {
    (void)prefix;
    for (int i = 0; i < n; i++) memcpy(&s->vi[idx[i]], &d[i], 4);
}

typedef struct Vector {
    uint32_t input, fcr31;
    uint32_t expected[5];
    uint32_t vf_expected[5];
} Vector;

static const Vector vectors[] = {
    {0x40200000u, 0u, {2u, 2u, 3u, 2u, 2u}, {2u, 2u, 3u, 2u, 0x7fffffffu}},
    {0xc0200000u, 0u, {0xfffffffeu, 0xfffffffeu, 0xfffffffeu, 0xfffffffdu, 0xfffffffeu}, {0xfffffffeu, 0xfffffffeu, 0xfffffffeu, 0xfffffffdu, 0x80000000u}},
    {0x40600000u, 0u, {4u, 3u, 4u, 3u, 4u}, {4u, 3u, 4u, 3u, 0x7fffffffu}},
    {0x40300000u, 1u, {3u, 2u, 3u, 2u, 2u}, {3u, 2u, 3u, 2u, 0x7fffffffu}},
    {0xc0300000u, 2u, {0xfffffffdu, 0xfffffffeu, 0xfffffffeu, 0xfffffffdu, 0xfffffffeu}, {0xfffffffdu, 0xfffffffeu, 0xfffffffeu, 0xfffffffdu, 0x80000000u}},
    {0x00000001u, 0u, {0u, 0u, 1u, 0u, 0u}, {0u, 0u, 1u, 0u, 0u}},
    {0x80000001u, 0u, {0u, 0u, 0u, 0xffffffffu, 0u}, {0u, 0u, 0u, 0xffffffffu, 0u}},
    {0x4effffffu, 0u, {0x7fffff80u, 0x7fffff80u, 0x7fffff80u, 0x7fffff80u, 0x7fffff80u}, {0x7fffff80u, 0x7fffff80u, 0x7fffff80u, 0x7fffff80u, 0x7fffffffu}},
    {0x4f000000u, 0u, {0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu}, {0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu}},
    {0xcf000000u, 0u, {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}, {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}},
    {0x7f7fffffu, 0u, {0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu}, {0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu}},
    {0xff7fffffu, 0u, {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}, {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}},
    {0x7f800000u, 0u, {0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu}, {0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu}},
    {0xff800000u, 0u, {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}, {0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u, 0x80000000u}},
    {0x7fc00000u, 3u, {0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu}, {0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu, 0x7fffffffu}},
};

int main(void) {
    const int host_modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    int failures = 0;
    for (unsigned m = 0; m < sizeof(host_modes) / sizeof(host_modes[0]); m++) {
        if (fesetround(host_modes[m]) != 0) return 2;
        for (unsigned i = 0; i < sizeof(vectors) / sizeof(vectors[0]); i++) {
            CpuState s = {0};
            s.fi[0] = vectors[i].input;
            s.vi[0] = vectors[i].input;
            s.fi[6] = 0xffffffffu;  /* cvt.s.w -1 is exact under every host mode. */
            s.fcr31 = vectors[i].fcr31;
            s.vfpuCtrl[0] = 0xe4u;
            s.vfpuCtrl[1] = 0xe4u;
            f_00001000(&s);
            const uint32_t got[5] = {s.r[8], s.r[9], s.r[10], s.r[11], s.r[12]};
            const uint32_t vf_got[5] = {s.vi[4], s.vi[8], s.vi[12], s.vi[16], s.vi[20]};
            for (unsigned j = 0; j < 5; j++) {
                if (got[j] != vectors[i].expected[j]) {
                    fprintf(stderr, "FAIL vector=%u mode=%u op=%u got=%08x want=%08x\\n",
                            i, m, j, got[j], vectors[i].expected[j]);
                    failures++;
                }
                if (vf_got[j] != vectors[i].vf_expected[j]) {
                    fprintf(stderr, "FAIL VFPU vector=%u mode=%u op=%u got=%08x want=%08x\\n",
                            i, m, j, vf_got[j], vectors[i].vf_expected[j]);
                    failures++;
                }
            }
            if (s.r[13] != 0xbf800000u) failures++;
        }
    }
    (void)fesetround(FE_TONEAREST);
    CpuState signed_max = {0};
    signed_max.fi[6] = 0x7fffffffu;
    f_00001000(&signed_max);
    if (signed_max.r[13] != 0x4f000000u) failures++;
    printf("generated_fp_convert: %s\\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
""",
                encoding="ascii",
            )

            chunks = sorted(work.glob("fp_convert_*.c"))
            exe = work / "fp_convert_test.exe"
            command = [
                CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                "-I", str(work), "-I", str(ROOT / "src" / "rt"),
            ]
            if os.name != "nt":
                command.extend([
                    "-fsanitize=undefined,float-cast-overflow",
                    "-fno-sanitize-recover=all",
                ])
            command.extend([
                str(work / "harness.c"), str(generated),
                *(str(path) for path in chunks), "-lm", "-o", str(exe),
            ])
            compiled = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
            ran = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
            self.assertIn("generated_fp_convert: PASS", ran.stdout)


def _run_generated_fixture(test, name, words, harness_main):
    """Run one synthetic-ELF scalar-FPU fixture through codegen + isolated harness."""
    assert CC is not None
    with tempfile.TemporaryDirectory(prefix=f"codegen_fp_scalar_{name}_") as tmp:
        work = Path(tmp)
        elf = work / f"{name}.elf"
        generated = work / f"{name}.c"
        elf.write_bytes(_synthetic_elf(words))

        env = dict(os.environ)
        env["HST_EXTRA_SPANS"] = ""
        result = subprocess.run(
            [sys.executable, str(CODEGEN), str(elf), str(generated), "--profile=hst"],
            cwd=ROOT, env=env, capture_output=True, text=True,
        )
        test.assertEqual(result.returncode, 0, result.stderr + result.stdout)

        (work / "recomp.h").write_text(ISOLATED_RECOMP_H, encoding="ascii")
        (work / "harness.c").write_text(
            f'#include "{name}_funcs.h"\n' + ISOLATED_STUBS_C + harness_main,
            encoding="ascii",
        )

        chunks = sorted(work.glob(f"{name}_*.c"))
        exe = work / f"{name}_test.exe"
        command = [
            CC, "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
            "-I", str(work), "-I", str(ROOT / "src" / "rt"),
        ]
        if os.name != "nt":
            command.extend([
                "-fsanitize=undefined,float-cast-overflow",
                "-fno-sanitize-recover=all",
            ])
        command.extend([
            str(work / "harness.c"), str(generated),
            *(str(path) for path in chunks), "-lm", "-o", str(exe),
        ])
        compiled = subprocess.run(command, capture_output=True, text=True)
        test.assertEqual(compiled.returncode, 0, compiled.stderr + compiled.stdout)
        ran = subprocess.run([str(exe)], capture_output=True, text=True)
        test.assertEqual(ran.returncode, 0, ran.stderr + ran.stdout)
        return (work / generated.name).read_text(encoding="ascii") + "\n".join(
            path.read_text(encoding="ascii") for path in chunks
        )


# Guest FCR31 field layout pinned by the PSP_HARDWARE-captured pspautotests fpu
# suite: RM bits [1:0], FCC0 bit 23, FS bit 24. Fixture expectations are exact
# correctly-rounded binary32 words derived independently with rational
# arithmetic in this repository; the mul.s row reproduces the hardware-published
# RM anchors (18.386576 under RN/RP, 18.386574 under RZ/RM).

_CELL1_MAIN = """\
#include <fenv.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* All scalar operations execute through the scoped-environment helpers,
     * so results must be correct regardless of the ambient host mode; the
     * harness still pins FE_TONEAREST so expectations are deterministic. */
    if (fesetround(FE_TONEAREST) != 0) return 2;
    int failures = 0;
    static const uint32_t exp_mul[4] = {0x419317b5u, 0x419317b4u, 0x419317b5u, 0x419317b4u};
    static const uint32_t exp_add[4] = {0x3f800000u, 0x3f800000u, 0x3f800001u, 0x3f800000u};
    static const uint32_t exp_sub[4] = {0x3f7ffffeu, 0x3f7ffffeu, 0x3f7fffffu, 0x3f7ffffeu};
    static const uint32_t exp_div[4] = {0x3eaaaaabu, 0x3eaaaaaau, 0x3eaaaaabu, 0x3eaaaaaau};
    const uint32_t *expected[4] = {exp_mul, exp_add, exp_sub, exp_div};
    uint32_t seen[4][4];
    for (unsigned m = 0; m < 4; m++) {
        CpuState s = {0};
        s.fi[0] = 0x3e97d668u;
        s.fi[1] = 0x42780000u;
        s.fi[2] = 0x3f800000u;
        s.fi[3] = 0x33800000u;
        s.fi[4] = 0x3f800000u;
        s.fi[5] = 0x33c00000u;
        s.fi[6] = 0x3f800000u;
        s.fi[7] = 0x40400000u;
        s.fcr31 = 0;
        s.r[8] = m;                       /* guest ctc1 installs RM=m */
        f_00001000(&s);
        seen[m][0] = s.r[9];   /* mul.s */
        seen[m][1] = s.r[10];  /* add.s */
        seen[m][2] = s.r[11];  /* sub.s */
        seen[m][3] = s.r[12];  /* div.s */
        for (unsigned op = 0; op < 4; op++) {
            if (seen[m][op] != expected[op][m]) {
                fprintf(stderr, "FAIL cell1 op=%u rm=%u got=%08x want=%08x\\n",
                        op, m, seen[m][op], expected[op][m]);
                failures++;
            }
        }
    }
    /* Vacuity guards: every op must discriminate at least two rounding modes,
     * and each mode sweep must not silently collapse to one value. */
    for (unsigned op = 0; op < 4; op++) {
        unsigned distinct = 1;
        for (unsigned m = 1; m < 4; m++) {
            if (seen[m][op] != seen[0][op]) distinct++;
        }
        if (distinct < 2) {
            fprintf(stderr, "FAIL cell1 vacuity op=%u all modes produced %08x\\n",
                    op, seen[0][op]);
            failures++;
        }
    }
    /* Executor diversity: the guest-RN cell must agree with a plain host-native
     * expression compiled from this very harness (not the same executor twice). */
    {
        float a, b;
        uint32_t bits;
        a = 0.29655766f; b = 62.0f;
        float ref = a * b;
        memcpy(&bits, &ref, sizeof(bits));
        if (bits != seen[0][0]) {
            fprintf(stderr, "FAIL cell1 executor-diversity host=%08x guest=%08x\\n",
                    bits, seen[0][0]);
            failures++;
        }
    }
    printf("generated_fp_scalar_rm: %s\\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
"""

_CELL2_MAIN = """\
#include <fenv.h>
#include <stdio.h>

int main(void) {
    if (fesetround(FE_TONEAREST) != 0) return 2;
    int failures = 0;
    struct Case { uint32_t fcr31; uint32_t pos; uint32_t neg; };
    static const struct Case cases[] = {
        {0x00000000u, 0x00400000u, 0x80400000u},  /* FS=0: gradual underflow   */
        {0x01000000u, 0x00000000u, 0x80000000u},  /* FS=1: flush, sign kept    */
    };
    uint32_t got_pos[2], got_neg[2];
    for (unsigned i = 0; i < 2; i++) {
        CpuState s = {0};
        s.fi[16] = 0x00800000u;   /* smallest normal  2^-126       */
        s.fi[17] = 0x3f000000u;   /* exactly 0.5                   */
        s.fi[18] = 0x80800000u;   /* negative smallest normal      */
        s.r[8] = cases[i].fcr31;  /* guest ctc1 installs FS        */
        f_00001000(&s);
        got_pos[i] = s.r[9];
        got_neg[i] = s.r[10];
        if (got_pos[i] != cases[i].pos) {
            fprintf(stderr, "FAIL cell2 fs=%u pos got=%08x want=%08x\\n",
                    i, got_pos[i], cases[i].pos);
            failures++;
        }
        if (got_neg[i] != cases[i].neg) {
            fprintf(stderr, "FAIL cell2 fs=%u neg got=%08x want=%08x\\n",
                    i, got_neg[i], cases[i].neg);
            failures++;
        }
    }
    if (got_pos[0] == got_pos[1]) {
        fprintf(stderr, "FAIL cell2 vacuity FS did not discriminate (%08x)\\n", got_pos[0]);
        failures++;
    }
    /* Signed zero preservation on the flushed negative product. */
    if (cases[1].neg != 0x80000000u || got_neg[1] != 0x80000000u) {
        fprintf(stderr, "FAIL cell2 flushed sign lost (%08x)\\n", got_neg[1]);
        failures++;
    }
    printf("generated_fp_scalar_fs: %s\\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
"""

_CELL3_MAIN = """\
#include <fenv.h>
#include <stdio.h>

int main(void) {
    if (fesetround(FE_TONEAREST) != 0) return 2;
    int failures = 0;
    CpuState s = {0};
    s.fi[20] = 0x3f800000u;   /* 1.0 */
    s.fi[21] = 0x40000000u;   /* 2.0 */
    s.r[8] = 0;
    f_00001000(&s);
    /* r9:  FCR31 after c.lt.s 1.0<2.0 -> FCC0 must observe TRUE  */
    if (((s.r[9] >> 23) & 1u) != 1u) {
        fprintf(stderr, "FAIL cell3 fcc0 after true compare: %08x\\n", s.r[9]);
        failures++;
    }
    if ((s.r[9] & 3u) != 0u) {
        fprintf(stderr, "FAIL cell3 compare perturbed RM: %08x\\n", s.r[9]);
        failures++;
    }
    /* r10: FCR31 after c.lt.s 2.0<1.0 -> FCC0 must observe FALSE */
    if (((s.r[10] >> 23) & 1u) != 0u) {
        fprintf(stderr, "FAIL cell3 fcc0 after false compare: %08x\\n", s.r[10]);
        failures++;
    }
    /* bc1t taken over both skip markers */
    if (s.r[12] == 0x0000deadu) {
        fprintf(stderr, "FAIL cell3 branch-on-true-compare not taken\\n");
        failures++;
    }
    /* Second bc1t follows a GUEST ctc1 of bit 23 only: the branch must honor
     * the architecturally-written FCC0. */
    if (s.r[13] == 0x0000beefu) {
        fprintf(stderr, "FAIL cell3 branch after ctc1 FCC0=1 not taken\\n");
        failures++;
    }
    /* Coherence: the cached condition mirrors the architectural FCC0 write. */
    if (s.fpcond != 1u) {
        fprintf(stderr, "FAIL cell3 fpcond stale after ctc1 FCC0 (%u)\\n", s.fpcond);
        failures++;
    }
    printf("generated_fp_scalar_fcc0: %s\\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
"""

_CELL4_MAIN = """\
#include <fenv.h>
#include <stdio.h>

int main(void) {
    if (fesetround(FE_TONEAREST) != 0) return 2;
    int failures = 0;
    uint32_t rn_pos = 0xFFFFFFFFu, rp_pos = 0xFFFFFFFFu;
    uint32_t rn_neg = 0xFFFFFFFFu, rm_neg = 0xFFFFFFFFu;
    struct Case { unsigned rm; int negative; };
    for (unsigned i = 0; i < 4; i++) {
        static const struct Case cases[] = {{0, 0}, {2, 0}, {0, 1}, {3, 1}};
        CpuState s = {0};
        s.fi[23] = 0x01000001u;   /* word 16777217  = 2^24+1  */
        s.fi[25] = 0xfeffffffu;   /* word -16777217            */
        s.r[8] = cases[i].rm;
        f_00001000(&s);
        if (!cases[i].negative) {
            if (cases[i].rm == 0) rn_pos = s.r[9];
            else rp_pos = s.r[9];
        } else {
            if (cases[i].rm == 0) rn_neg = s.r[10];
            else rm_neg = s.r[10];
        }
    }
    if (rn_pos != 0x4b800000u) { fprintf(stderr, "FAIL cell4 RN pos %08x\\n", rn_pos); failures++; }
    if (rp_pos != 0x4b800001u) { fprintf(stderr, "FAIL cell4 RP pos %08x\\n", rp_pos); failures++; }
    if (rn_neg != 0xcb800000u) { fprintf(stderr, "FAIL cell4 RN neg %08x\\n", rn_neg); failures++; }
    if (rm_neg != 0xcb800001u) { fprintf(stderr, "FAIL cell4 RM neg %08x\\n", rm_neg); failures++; }
    if (rn_pos == rp_pos) {
        fprintf(stderr, "FAIL cell4 vacuity guest RM ignored by cvt.s.w\\n");
        failures++;
    }
    if (rn_neg == rm_neg) {
        fprintf(stderr, "FAIL cell4 vacuity negative sweep collapsed\\n");
        failures++;
    }
    printf("generated_fp_scalar_cvtsw: %s\\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
"""


_CELL_HOSTILE_MAIN = """\
#include <fenv.h>
#include <stdio.h>
#include <xmmintrin.h>

int main(void) {
    if (fesetround(FE_TONEAREST) != 0) return 2;
    int failures = 0;
    struct Hostile { const char *name; uint32_t set; uint32_t clear; };
    static const struct Hostile bases[] = {
        /* Labels name the HOST x86 MXCSR RC field encoding, not MIPS RM. */
        {"RC=x86 -inf", 1u << 13, 0u},
        {"RC=x86 +inf", 2u << 13, 0u},
        {"RC=x86 zero", 3u << 13, 0u},
        {"FTZ",     1u << 15, 0u},
        {"DAZ",     1u << 6,  0u},
    };
    for (unsigned i = 0; i < 5; i++) {
        const uint32_t base = _mm_getcsr();
        const uint32_t hostile = (base | bases[i].set) & ~bases[i].clear;
        _mm_setcsr(hostile);
        CpuState s = {0};
        s.fi[0] = 0x3e97d668u;    /* RM-sensitive mul operands */
        s.fi[1] = 0x42780000u;
        s.fi[16] = 0x00800000u;   /* smallest normal */
        s.fi[17] = 0x3f000000u;   /* 0.5: FS=0 must stay gradual */
        s.r[8] = 0u;              /* guest ctc1: RN, gradual */
        f_00001000(&s);
        if (s.r[9] != 0x419317b5u) {
            fprintf(stderr, "FAIL hostile %s mul got=%08x want=419317b5\\n",
                    bases[i].name, s.r[9]);
            failures++;
        }
        if (s.r[10] != 0x00400000u) {
            fprintf(stderr, "FAIL hostile %s gradual got=%08x want=00400000\\n",
                    bases[i].name, s.r[10]);
            failures++;
        }
        if (_mm_getcsr() != hostile) {
            fprintf(stderr, "FAIL hostile %s mxcsr before=%08x after=%08x\\n",
                    bases[i].name, hostile, _mm_getcsr());
            failures++;
        }
        _mm_setcsr(base);
    }
    printf("generated_fp_scalar_hostile: %s\\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
"""


_CELL_INFZERO_MAIN = """\
#include <fenv.h>
#include <stdio.h>
#include <xmmintrin.h>

int main(void) {
    if (fesetround(FE_TONEAREST) != 0) return 2;
    int failures = 0;
    struct Base { const char *name; uint32_t set; uint32_t clear; };
    static const struct Base bases[] = {
        {"DAZ=1",     1u << 6, 0u},
        {"DAZ=0 ctl", 0u,      0u},
    };
    static const struct { const char *what; unsigned reg; uint32_t want; } rows[] = {
        {"+inf * +minsub", 9,  0x7f800000u},
        {"+minsub * +inf", 10, 0x7f800000u},
        {"-inf * +minsub", 11, 0xff800000u},
        {"+inf * +0",      12, 0x7fc00000u},
        {"+inf * -0",      13, 0x7fc00000u},
        {"+0 * +inf",      14, 0x7fc00000u},
        {"-0 * -inf",      15, 0x7fc00000u},
    };
    for (unsigned i = 0; i < 2; i++) {
        const uint32_t base = _mm_getcsr();
        const uint32_t hostile = (base | bases[i].set) & ~bases[i].clear;
        _mm_setcsr(hostile);
        CpuState s = {0};
        s.fi[0] = 0x7f800000u;   /* +inf           */
        s.fi[1] = 0x00000001u;   /* min subnormal  */
        s.fi[2] = 0xff800000u;   /* -inf           */
        s.fi[3] = 0x00000000u;   /* exact +0       */
        s.fi[4] = 0x80000000u;   /* exact -0       */
        s.r[8] = 0u;
        f_00001000(&s);
        for (unsigned r = 0; r < 7; r++) {
            if (s.r[rows[r].reg] != rows[r].want) {
                fprintf(stderr, "FAIL %s %s got=%08x want=%08x\\n",
                        bases[i].name, rows[r].what, s.r[rows[r].reg], rows[r].want);
                failures++;
            }
        }
        if (_mm_getcsr() != hostile) {
            fprintf(stderr, "FAIL %s mxcsr before=%08x after=%08x\\n",
                    bases[i].name, hostile, _mm_getcsr());
            failures++;
        }
        _mm_setcsr(base);
    }
    printf("generated_fp_scalar_infzero: %s\\n", failures == 0 ? "PASS" : "FAIL");
    return failures == 0 ? 0 : 1;
}
"""


@unittest.skipUnless(CC, "gcc is required for the generated-C conversion regression")
class GeneratedScalarFcr31Tests(unittest.TestCase):
    """Guest-visible scalar COP1 semantics driven entirely through generated code.

    Four cells from the research IF-1 packet, each exercising a real MIPS
    instruction sequence (including the guest `ctc1` that installs FCR31):
      CELL 1 - non-default RM directs add/sub/mul/div.s results.
      CELL 2 - FCR31.FS gates gradual underflow vs flush-to-zero.
      CELL 3 - c.cond.s results are visible to cfc1 as coherent FCC0 and to
               branch-on-FP-condition.
      CELL 4 - cvt.s.w honors guest RM near the float precision boundary.

    Expectations are raw IEEE-754 words derived independently by rational
    arithmetic in this repository; the mul.s row additionally reproduces the
    public PSP_HARDWARE RM anchors. The production interpreter floor has no
    COP1 support yet, so these cells are AOT-executor scoped by design.
    """

    def test_cell1_rounding_modes_direct_scalar_arithmetic(self):
        words: list[int] = [
            _ctc1(8),                                   # ctc1 $31, t0
            _fps3(1, 0, 8, 0x02), _mfc1(9, 8),          # mul.s f8,f0,f1
            _fps3(3, 2, 8, 0x00), _mfc1(10, 8),         # add.s f8,f2,f3
            _fps3(5, 4, 8, 0x01), _mfc1(11, 8),         # sub.s f8,f4,f5
            _fps3(7, 6, 8, 0x03), _mfc1(12, 8),         # div.s f8,f6,f7
            0x03E00008, 0x00000000,                     # jr ra; nop
        ]
        emitted = _run_generated_fixture(self, "fp_scalar_rm", words, _CELL1_MAIN)
        self.assertNotIn("sr_fpu_scalar_fast", emitted)  # fast path removed for correctness
        self.assertIn("sr_fpu_mul_s", emitted)
        self.assertIn("sr_fpu_add_s", emitted)
        self.assertIn("sr_fpu_sub_s", emitted)
        self.assertIn("sr_fpu_div_s", emitted)

    def test_cell2_fs_bit_gates_gradual_underflow(self):
        words: list[int] = [
            _ctc1(8),
            _fps3(17, 16, 8, 0x02), _mfc1(9, 8),        # mul.s f8,f16,f17
            _fps3(17, 18, 8, 0x02), _mfc1(10, 8),       # mul.s f9,f18,f17
            0x03E00008, 0x00000000,
        ]
        emitted = _run_generated_fixture(self, "fp_scalar_fs", words, _CELL2_MAIN)
        self.assertIn("sr_fpu_mul_s", emitted)

    def test_cell3_cond_compare_coherent_fcc0_and_branch(self):
        words: list[int] = [
            _ccond(21, 20, 0xC),                        # c.lt.s f20,f21 (true)
            _cfc1(9),                                   # r9  = FCR31
            _ccond(20, 21, 0xC),                        # c.lt.s f21,f20 (false)
            _cfc1(10),                                  # r10 = FCR31
            _ccond(21, 20, 0xC),
            _bc1(1, 2),                                 # bc1t over nop+ori
            0x00000000,                                 # delay slot
            _ori(12, 0xDEAD),                           # skipped when taken
            _lui(8, 0x0080),                            # t0 = 0x00800000
            _ctc1(8),                                   # guest writes FCC0 only
            _bc1(1, 2),
            0x00000000,
            _ori(13, 0xBEEF),                           # skipped when taken
            0x03E00008, 0x00000000,
        ]
        emitted = _run_generated_fixture(self, "fp_scalar_fcc0", words, _CELL3_MAIN)
        self.assertIn(">> 23", emitted)                 # FCC0 lands in FCR31

    def test_cell4_cvt_sw_honors_guest_rounding_mode(self):
        words: list[int] = [
            _ctc1(8),
            _fpw(23, 22), _mfc1(9, 22),                 # cvt.s.w f22,f23
            _fpw(25, 24), _mfc1(10, 24),                # cvt.s.w f24,f25
            0x03E00008, 0x00000000,
        ]
        emitted = _run_generated_fixture(self, "fp_scalar_cvtsw", words, _CELL4_MAIN)
        self.assertIn("sr_fpu_cvt_s_w", emitted)

    def test_hostile_host_environment_isolated_from_default_guest_ops(self):
        """Guest RM=RN/FS=0 must be isolated from a hostile host FP environment.

        This is the fast-path disposition guard: with the native fast path
        removed (review Disposition A), even default-state guests execute
        through the scoped helpers, so ambient host RC/FTZ/DAZ cannot change
        guest result bits and the caller's MXCSR must survive bit-for-bit.
        """
        words: list[int] = [
            _ctc1(8),                                   # guest installs RM=0/FS=0
            _fps3(1, 0, 8, 0x02), _mfc1(9, 8),          # mul.s (RM-sensitive vector)
            _fps3(17, 16, 8, 0x02), _mfc1(10, 8),       # mul.s min-normal * 0.5 (FS=0 gradual)
            0x03E00008, 0x00000000,
        ]
        emitted = _run_generated_fixture(
            self, "fp_scalar_hostile", words, _CELL_HOSTILE_MAIN)
        self.assertNotIn("sr_fpu_scalar_fast", emitted)  # fast path stays gone

    def test_mul_inf_zero_classifier_raw_bit_under_hostile_daz(self):
        """The inf*0 pre-classifier must classify from RAW BITS, not FP compares.

        A floating `_b == 0.0f` precheck executes outside the scoped window;
        under hostile ambient DAZ=1 a real subnormal operand compares equal to
        zero, sending inf * min-subnormal down the canonical-qNaN path. Raw
        exponent/mantissa classification is environment-blind. Rows:
          1. +inf * +min-subnormal, DAZ=1 -> +inf       (fails on FP compare)
          2. +min-subnormal * +inf, DAZ=1 -> +inf       (fails on FP compare)
          3. -inf * +min-subnormal, DAZ=1 -> -inf       (fails on FP compare)
          4. +inf * +0            -> canonical 0x7fc00000
          5. +inf * -0            -> canonical 0x7fc00000
          6. symmetric zero*inf forms -> canonical 0x7fc00000
          7. DAZ=0 controls proving identical classification outcomes
        Every hostile row also asserts exact caller-MXCSR restoration.
        """
        words: list[int] = [
            _fps3(1, 0, 8, 0x02), _mfc1(9, 8),    # +inf * +min-subnormal
            _fps3(0, 1, 8, 0x02), _mfc1(10, 8),   # +min-subnormal * +inf
            _fps3(1, 2, 8, 0x02), _mfc1(11, 8),   # -inf * +min-subnormal
            _fps3(3, 0, 8, 0x02), _mfc1(12, 8),   # +inf * +0
            _fps3(4, 0, 8, 0x02), _mfc1(13, 8),   # +inf * -0
            _fps3(0, 3, 8, 0x02), _mfc1(14, 8),   # +0 * +inf
            _fps3(2, 4, 8, 0x02), _mfc1(15, 8),   # -0 * -inf
            0x03E00008, 0x00000000,
        ]
        emitted = _run_generated_fixture(
            self, "fp_scalar_infzero", words, _CELL_INFZERO_MAIN)
        self.assertIn("sr_fpu_mul_s", emitted)
        self.assertIn("0x7f800000u", emitted)         # raw-bit inf classification
        self.assertNotIn("isinf(_a)", emitted)        # no FP compare classifier


if __name__ == "__main__":
    unittest.main()
