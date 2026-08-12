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

            (work / "recomp.h").write_text(
                """\
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
#endif
""",
                encoding="ascii",
            )
            (work / "harness.c").write_text(
                """\
#include <fenv.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fp_convert_funcs.h"

void sr_register(uint32_t addr, void (*fn)(CpuState *)) { (void)addr; (void)fn; }
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


if __name__ == "__main__":
    unittest.main()
