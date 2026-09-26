#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 the Nakagawa Recomp authors

"""Tests for the GE's fail-closed handling of a non-finite vertex (issue #69).

A NaN bone matrix (or a NaN vertex normal) makes the skinned position or the lit colour
non-finite, and every comparison in the GE's primitive-acceptance test is false against
NaN, so such a vertex used to be accepted and rasterized with NaN edge functions or a NaN
pixel value. The PSP's own answer is NOT_MEASURED, so the GE drops the primitive and
counts it instead of inventing screen coverage.

The C harness below is synthetic (generated into a temp dir, never tracked): it feeds a
weighted VTYPE, lighting, a bone-matrix upload and one PRIM through the production GE list
walker in ``src/rt/ge.c`` with ``SR_GESTAT`` armed, in three modes:

* ``finite``    - identity bone and finite normals: the control that must still rasterize;
* ``nanbone``   - a NaN word in the bone translation: the position becomes non-finite;
* ``nannormal`` - a NaN vertex normal with lighting on: the position stays finite and the
  lit colour becomes non-finite. This is the case the GE used to rasterize, writing a
  whole triangle of pixels whose value came from a host (int)NaN conversion.
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
DOCS = ROOT / "docs"
CC = shutil.which("gcc") or shutil.which("cc") or shutil.which("clang")

HARNESS_C = r"""
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "recomp.h"
#include "debug.h"

uint8_t *g_mem;
CpuState *s_cpu = NULL;
int g_sr_heap_watch = 0;
int g_sr_metadata_watch = 0;
int g_hle_depth = 0;
int g_sr_last_writer_enabled = 0;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
uint32_t g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_limit = 0;
unsigned g_sr_store_context_count = 0;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;

static unsigned s_oor = 0;
void sr_oor(uint32_t a, uint32_t v, int store) { (void)a; (void)v; (void)store; s_oor++; }
void sr_heap_note_write(uint32_t a, uint32_t w, uint32_t v, uint32_t pc) {
    (void)a; (void)w; (void)v; (void)pc;
}
void sr_heap_note_bulk_write(uint32_t a, uint32_t w, uint32_t pc) {
    (void)a; (void)w; (void)pc;
}
void sr_add_mem_watch(uint32_t s, uint32_t e, const char *l) {
    (void)s; (void)e; (void)l;
}
void sr_note_mem_write(uint32_t a, uint32_t w, uint32_t v, uint32_t pc) {
    (void)a; (void)w; (void)v; (void)pc;
}
int sr_find_last_writer(uint32_t a, uint32_t w, uint32_t *pa, uint32_t *pw,
                        uint32_t *pv, uint32_t *pp) {
    (void)a; (void)w; (void)pa; (void)pw; (void)pv; (void)pp;
    return 0;
}
uint32_t sched_current_uid(void) { return 0; }
uint32_t sr_get_ge_status(void) { return 0; }
uint64_t SDL_GetTicksNS(void) { static uint64_t t = 0; return (t += 1000); }

extern void ge_set_frame(uint32_t frame);
extern uint32_t ge_run_list(uint32_t addr, int resume);

#define LIST1 0x08010000u
#define VERTS 0x08020000u
#define FB    0x04044000u
#define FBW   512u

static uint32_t s_pc;
static uint32_t f24(float f) { uint32_t u; memcpy(&u, &f, 4); return (u >> 8) & 0xFFFFFFu; }
static void W(uint32_t cmd, uint32_t data) {
    uint32_t w = (cmd << 24) | (data & 0xFFFFFFu);
    memcpy(SR_HOST(s_pc), &w, 4);
    s_pc += 4;
}
static void mat_upload(uint32_t num_cmd, uint32_t data_cmd, const float *m, int n) {
    W(num_cmd, 0);
    for (int i = 0; i < n; i++) W(data_cmd, f24(m[i]));
}
static void emit_vtx(uint32_t addr, float w, float u, float v, uint32_t col,
                     float nx, float ny, float nz, float x, float y, float z) {
    float f[10];
    f[0] = w; f[1] = u; f[2] = v; memcpy(&f[3], &col, 4);
    f[4] = nx; f[5] = ny; f[6] = nz; f[7] = x; f[8] = y; f[9] = z;
    memcpy(SR_HOST(addr), f, sizeof(f));
}

/* tc=float col=8888 nrm=float pos=float wt=float morph=1 through=0 idx=none */
#define VT (3u | (7u << 2) | (3u << 5) | (3u << 7) | (3u << 9))

static const float kIdent43[12] = { 1,0,0, 0,1,0, 0,0,1, 0,0,0 };
static const float kIdent44[16] = { 1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1 };

static float from_bits(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

static void emit_list(uint32_t list, const float *bone, int nrm_nan) {
    s_pc = list;
    W(0x14, 0);                       /* ORIGIN: offset := this word */
    W(0x10, 0);                       /* BASE */
    W(0x9C, 0x044000);                /* FRAMEBUFPTR -> 0x04044000 */
    W(0x9D, FBW);                     /* FRAMEBUFWIDTH */
    W(0xD2, 3);                       /* PIXFORMAT 8888 */
    W(0xD4, 0);                       /* SCISSOR1 */
    W(0xD5, (271u << 10) | 479u);     /* SCISSOR2 */
    W(0x17, 1);                       /* LIGHTINGENABLE */
    W(0x18, 1);                       /* LIGHTENABLE0: one directional light, so a NaN
                                       * vertex normal reaches ndl and the lit colour */
    W(0x5F, (1u << 8));               /* LIGHTTYPE0: (1<<8) = directional */
    W(0x63, f24(0.0f)); W(0x64, f24(0.0f)); W(0x65, f24(1.0f));   /* light 0 direction */
    W(0x8F, 0); W(0x90, 0); W(0x91, 0);              /* light 0 ambient colour  */
    W(0x92, 0x808080);                              /* light 0 diffuse colour  */
    W(0x1E, 0);                       /* TEXTUREMAPENABLE */
    W(0xA0, 0x030000);
    W(0xA8, (0x08u << 16) | 64u);
    W(0xB8, (6u << 8) | 6u);
    W(0xC3, 3);
    W(0x42, f24(240.0f)); W(0x43, f24(136.0f)); W(0x44, f24(1000.0f));
    W(0x45, f24(240.0f)); W(0x46, f24(136.0f)); W(0x47, f24(30000.0f));
    W(0x4C, 0); W(0x4D, 0);
    mat_upload(0x3A, 0x3B, kIdent43, 12);   /* WORLD */
    mat_upload(0x3C, 0x3D, kIdent43, 12);   /* VIEW */
    mat_upload(0x3E, 0x3F, kIdent44, 16);   /* PROJ */
    mat_upload(0x2A, 0x2B, bone, 12);       /* BONE0 */
    W(0x12, VT);
    W(0x01, VERTS - list);                  /* VADDR (ORIGIN-relative) */
    W(0x04, (3u << 16) | 3u);               /* PRIM: TRIANGLES, 3 vertices */
    W(0x0C, 0);                             /* END */
    float nz = nrm_nan ? from_bits(0x7fc00000u) : 1.0f;
    emit_vtx(VERTS + 0u * 40u, 1.0f, 0, 0, 0xFFFFFFFFu, 0, 0, nz, -0.5f, -0.5f, 0);
    emit_vtx(VERTS + 1u * 40u, 1.0f, 1, 0, 0xFFFFFFFFu, 0, 0, 1, 0.5f, -0.5f, 0);
    emit_vtx(VERTS + 2u * 40u, 1.0f, 0, 1, 0xFFFFFFFFu, 0, 0, 1, 0.0f, 0.5f, 0);
}

/* The framebuffer is seeded with a sentinel so "the rasterizer wrote a pixel" is
 * distinguishable from "the rasterizer wrote black": lighting with no enabled light
 * legitimately produces 0x00000000. */
#define SENTINEL 0x5a5a5a5au

static unsigned lit_pixels(void) {
    unsigned n = 0;
    for (uint32_t y = 0; y < 272; y++)
        for (uint32_t x = 0; x < FBW; x++) {
            uint32_t w;
            memcpy(&w, SR_HOST(FB + (y * FBW + x) * 4u), 4);
            if (w != SENTINEL) n++;
        }
    return n;
}

static void clear_fb(void) {
    for (uint32_t i = 0; i < 272u * FBW; i++) {
        uint32_t w = SENTINEL;
        memcpy(SR_HOST(FB + i * 4u), &w, 4);
    }
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "finite";
    float bone[12];
    memcpy(bone, kIdent43, sizeof(bone));
    if (strcmp(mode, "nanbone") == 0) bone[3] = from_bits(0x7fc00000u);

    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) return 2;
    g_mem = arena + 0x08000000u;

    emit_list(LIST1, bone, strcmp(mode, "nannormal") == 0);
    clear_fb();
    ge_set_frame(60);
    if (ge_run_list(LIST1, 0) != 0) { printf("FAIL: list did not END\n"); return 1; }
    printf("LIT=%u\n", lit_pixels());
    /* GESTAT prints on the frame boundary, so step to the next multiple of 60 to flush
     * the counters this list accumulated. */
    ge_set_frame(120);
    printf("OOR=%u\nDONE\n", s_oor);
    free(arena);
    return 0;
}
"""


def _compile(tmp: Path) -> Path:
    assert CC is not None
    harness_c = tmp / "ge_nonfinite_harness.c"
    harness_c.write_text(HARNESS_C, encoding="utf-8")
    exe = tmp / "ge_nonfinite_harness.exe"
    result = subprocess.run(
        [CC, "-std=c11", "-O1", "-Isrc/rt", "-DSR_SDL3VK", "-o", os.fspath(exe),
         os.fspath(harness_c), os.fspath(RT / "ge.c"), os.fspath(RT / "ge_capture.c"),
         os.fspath(RT / "perf.c"), "-lm"],
        capture_output=True, text=True, cwd=ROOT,
    )
    if result.returncode != 0:
        raise AssertionError("non-finite GE harness did not compile:\n" + result.stderr)
    return exe


def _run(exe: Path, mode: str) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    env["SR_GESTAT"] = "1"
    for name in ("SR_RTRACE", "SR_GE_TRANSITION_TRACE", "SR_NAN_TRAP", "SR_PIXWHO"):
        env.pop(name, None)
    return subprocess.run([os.fspath(exe), mode], capture_output=True, text=True, env=env)


def _stat(stderr: str, frame: int) -> dict:
    rows = dict(re.findall(r"GESTAT\+ f=(\d+) (.*)", stderr))
    line = rows.get(str(frame), "")
    return dict(re.findall(r"(\w+)=(\d+)", line)) if line else {}


@unittest.skipUnless(CC, "no C compiler on PATH")
class TestNonFiniteVertex(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = Path(tempfile.mkdtemp(prefix="genonfinite_"))
        cls.exe = _compile(cls.tmp)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def test_finite_bone_still_draws(self):
        result = _run(self.exe, "finite")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DONE", result.stdout)
        self.assertEqual(re.search(r"OOR=(\d+)", result.stdout).group(1), "0",
                         "fixture caused out-of-range guest access")
        lit = int(re.search(r"LIT=(\d+)", result.stdout).group(1))
        self.assertGreater(lit, 100, "control: a finite bone matrix must still rasterize")
        self.assertEqual(_stat(result.stderr, 120).get("nonfinite"), "0",
                         "a finite bone matrix must not count as non-finite")

    def test_nan_bone_drops_the_primitive(self):
        result = _run(self.exe, "nanbone")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DONE", result.stdout)
        lit = int(re.search(r"LIT=(\d+)", result.stdout).group(1))
        self.assertEqual(lit, 0,
                         "a non-finite skinned position must not rasterize "
                         f"(it wrote {lit} pixels)")
        self.assertEqual(_stat(result.stderr, 120).get("nonfinite"), "1",
                         "the dropped triangle must be counted for GESTAT")

    def test_nan_lit_colour_drops_the_primitive(self):
        # The position stays finite here, so only the lit colour is non-finite: this is the
        # case the GE used to rasterize, writing pixels from a host (int)NaN conversion.
        result = _run(self.exe, "nannormal")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("DONE", result.stdout)
        lit = int(re.search(r"LIT=(\d+)", result.stdout).group(1))
        self.assertEqual(lit, 0,
                         "a non-finite lit colour must not rasterize "
                         f"(it wrote {lit} pixels)")
        self.assertEqual(_stat(result.stderr, 120).get("nonfinite"), "1",
                         "the dropped triangle must be counted for GESTAT")

    def test_docs_name_the_boundary(self):
        row = (DOCS / "COMPATIBILITY.md").read_text(encoding="utf-8")
        ge_row = [line for line in row.splitlines() if line.startswith("| Graphics / GE")]
        self.assertEqual(len(ge_row), 1, "COMPATIBILITY.md must carry one Graphics / GE row")
        text = ge_row[0]
        self.assertIn("non-finite", text)
        self.assertIn("#69", text)
        oracle = (DOCS / "HARDWARE_ORACLE.md").read_text(encoding="utf-8")
        needle = "on-finite vertex position and lit colour"
        self.assertIn(needle, oracle)
        entry = oracle.split(needle, 1)[1]
        self.assertIn("NOT_MEASURED", entry[:1200])


if __name__ == "__main__":
    unittest.main()
