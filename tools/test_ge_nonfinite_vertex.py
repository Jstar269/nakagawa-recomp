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

Those three are transform mode, where ge.c already carries the strong upstream verdict
(CVtx::nf: clip position, projected screen position and lit colour). The modes below cover
the inputs that verdict cannot see, and each is driven twice: once with NO GPU backend
(so the software rasterizer decides) and once with a recording backend registered through
the real ``ge_set_gpu_hooks`` capture seam the Vulkan rasterizer uses (so the test can ask
"would the Vulkan path have been handed this primitive?"). Both must reach the same verdict:

* ``throughtri`` / ``throughspr`` - THROUGH-mode vertices (raw screen coordinates read
  straight out of guest VRAM, never finiteness-checked before) carrying a NaN float. This
  is the case the GE used to hand to BOTH rasterizers unchecked: on Vulkan a NaN
  gl_Position has an undefined fixed-function clipper verdict, so the primitive is not
  reliably clipped away and the rasterizer can cover an unbounded area.
* ``throughok``  - the same lists with finite coordinates: the control that must still be
  submitted to the GPU seam and still rasterize in software.
* ``patchnan``   - a Bezier patch (patch/spline tessellation, a separate submit path that
  has no CVtx::nf to consult) with a NaN bone matrix.
* ``predicate``  - prints the verdict of ``ge_vtx_finite`` (ge_shared.h), the single
  predicate BOTH rasterizers apply, over a table of inputs.

``gpuprim`` in the GESTAT+ line counts primitives a GPU backend took: without it a
GPU-rasterized scene reports zero 3D primitives, because a taken primitive returns before
the tri2d/tri3d/spr/px counters.
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
#include "ge_shared.h"

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

/* Recording stand-in for the Vulkan rasterizer, registered through the real capture seam
 * (GeGpuHooks is what src/rt/gpu_sdl3vk/ge_gpu.c fills in). It counts every primitive the
 * GE offers to the GPU path and takes it, so the software rasterizer never runs: after one
 * list, LIT must be 0 and HOOK tells the Vulkan path's verdict. */
static unsigned s_hook_calls = 0;
static int hook_tri(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    (void)a; (void)b; (void)c; (void)persp; s_hook_calls++; return 1;
}
static int hook_sprite(const GeVtx *a, const GeVtx *b, int persp) {
    (void)a; (void)b; (void)persp; s_hook_calls++; return 1;
}
static int hook_line(const GeVtx *a, const GeVtx *b, int persp) {
    (void)a; (void)b; (void)persp; s_hook_calls++; return 1;
}
static int hook_point(const GeVtx *a, int persp) {
    (void)a; (void)persp; s_hook_calls++; return 1;
}
static void hook_register(void) {
    GeGpuHooks h;
    memset(&h, 0, sizeof(h));
    h.tri = hook_tri; h.sprite = hook_sprite; h.line = hook_line; h.point = hook_point;
    ge_set_gpu_hooks(&h);
}

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

/* tc=float col=8888 pos=float through=1 idx=none. pos_fmt=3 makes a through vertex carry
 * raw float screen coordinates, so a NaN word written by the guest lands in the vertex
 * exactly as a NaN float would (STRIDE 24: u, v, rgba8888, x, y, z). */
#define VT_THROUGH (3u | (7u << 2) | (3u << 7) | (1u << 23))
#define THROUGH_STRIDE 24u

static void emit_through_vtx(uint32_t addr, float x, float y, float z) {
    float f[6];
    f[0] = 0.0f; f[1] = 0.0f;
    uint32_t col = 0xFFFFFFFFu;
    memcpy(&f[2], &col, 4);
    f[3] = x; f[4] = y; f[5] = z;
    memcpy(SR_HOST(addr), f, sizeof(f));
}

static void emit_through_list(uint32_t list, uint32_t prim_type, uint32_t count, int nan_v1) {
    float nan_x = from_bits(0x7fc00000u);
    s_pc = list;
    W(0x14, 0);                       /* ORIGIN */
    W(0x10, 0);                       /* BASE */
    W(0x9C, 0x044000);                /* FRAMEBUFPTR -> 0x04044000 */
    W(0x9D, FBW);                     /* FRAMEBUFWIDTH */
    W(0xD2, 3);                       /* PIXFORMAT 8888 */
    W(0xD4, 0);                       /* SCISSOR1 */
    W(0xD5, (271u << 10) | 479u);     /* SCISSOR2 */
    W(0x1E, 0);                       /* TEXTUREMAPENABLE */
    W(0xA0, 0x030000);
    W(0xA8, (0x08u << 16) | 64u);
    W(0xB8, (6u << 8) | 6u);
    W(0xC3, 3);
    W(0x42, f24(240.0f)); W(0x43, f24(136.0f)); W(0x44, f24(1000.0f));
    W(0x45, f24(240.0f)); W(0x46, f24(136.0f)); W(0x47, f24(30000.0f));
    W(0x4C, 0); W(0x4D, 0);
    W(0x12, VT_THROUGH);
    W(0x01, VERTS - list);            /* VADDR (ORIGIN-relative) */
    W(0x04, (prim_type << 16) | count);
    W(0x0C, 0);                       /* END */
    /* Through-mode x/y are raw screen pixels, not model units: a pixel-scale quad. */
    float x1 = nan_v1 ? nan_x : 160.0f;
    if (prim_type == 6) {             /* SPRITES: p0 lower-left, p1 upper-right */
        emit_through_vtx(VERTS + 0u * THROUGH_STRIDE, 80.0f, 60.0f, 0.0f);
        emit_through_vtx(VERTS + 1u * THROUGH_STRIDE, x1, 200.0f, 0.0f);
    } else {                          /* TRIANGLES: NaN in the first vertex's x */
        emit_through_vtx(VERTS + 0u * THROUGH_STRIDE, x1, 60.0f, 0.0f);
        emit_through_vtx(VERTS + 1u * THROUGH_STRIDE, 320.0f, 60.0f, 0.0f);
        emit_through_vtx(VERTS + 2u * THROUGH_STRIDE, 200.0f, 200.0f, 0.0f);
    }
}

/* Bezier patch: a 4x4 control grid, PATCHDIVISION 1x1 and PATCHPRIMITIVE 2 (triangles),
 * so draw_patch submits two triangles through submit_model_triangle - the tessellation
 * submit path, which has no CVtx::nf verdict of its own to consult. */
static void emit_patch_list(uint32_t list, const float *bone) {
    s_pc = list;
    W(0x14, 0);                       /* ORIGIN */
    W(0x10, 0);                       /* BASE */
    W(0x9C, 0x044000);
    W(0x9D, FBW);
    W(0xD2, 3);
    W(0xD4, 0);
    W(0xD5, (271u << 10) | 479u);
    W(0x1E, 0);
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
    W(0x01, VERTS - list);
    W(0x36, (1u << 8) | 1u);         /* PATCHDIVISION 1x1 */
    W(0x37, 2);                       /* PATCHPRIMITIVE: triangles */
    W(0x05, 0x404);                   /* BEZIER 4x4 control points */
    W(0x0C, 0);                       /* END */
    for (int v = 0; v < 4; v++)
        for (int u = 0; u < 4; u++)
            emit_vtx(VERTS + (uint32_t)(v * 4 + u) * 40u, 1.0f, 0, 0, 0xFFFFFFFFu,
                     0, 0, 1, u < 2 ? -0.5f : 0.5f, v < 2 ? -0.5f : 0.5f, 0);
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

/* The shared predicate both rasterizers apply, over a table of inputs: one finite control
 * and one non-finite value in each of the four fields it covers. */
static void print_predicates(void) {
    const float nan = from_bits(0x7fc00000u), inf = from_bits(0x7f800000u);
    struct { const char *name; float x, y, z, rw; int want; } t[] = {
        { "finite", 1.0f, 2.0f, 3.0f, 1.0f, 1 },
        { "nan_x",  nan,  2.0f, 3.0f, 1.0f, 0 },
        { "inf_y",  1.0f, inf,  3.0f, 1.0f, 0 },
        { "inf_z",  1.0f, 2.0f, -inf, 1.0f, 0 },
        { "nan_rw", 1.0f, 2.0f, 3.0f, nan,  0 },
    };
    for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        GeVtx v;
        memset(&v, 0, sizeof(v));
        v.x = t[i].x; v.y = t[i].y; v.z = t[i].z; v.rw = t[i].rw;
        int got = ge_vtx_finite(&v) ? 1 : 0;
        printf("PRED %s %d want=%d\n", t[i].name, got, t[i].want);
    }
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "finite";
    int with_gpu = (argc > 2) && strcmp(argv[2], "hook") == 0;
    float bone[12];
    memcpy(bone, kIdent43, sizeof(bone));
    if (strcmp(mode, "nanbone") == 0 || strcmp(mode, "patchnan") == 0)
        bone[3] = from_bits(0x7fc00000u);

    if (strcmp(mode, "predicate") == 0) { print_predicates(); printf("DONE\n"); return 0; }

    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) return 2;
    g_mem = arena + 0x08000000u;

    if (strcmp(mode, "throughtri") == 0 || strcmp(mode, "throughok") == 0)
        emit_through_list(LIST1, 3, 3, strcmp(mode, "throughtri") == 0);
    else if (strcmp(mode, "throughspr") == 0)
        emit_through_list(LIST1, 6, 2, 1);
    else if (strcmp(mode, "patchnan") == 0)
        emit_patch_list(LIST1, bone);
    else
        emit_list(LIST1, bone, strcmp(mode, "nannormal") == 0);
    if (with_gpu) hook_register();
    clear_fb();
    ge_set_frame(60);
    if (ge_run_list(LIST1, 0) != 0) { printf("FAIL: list did not END\n"); return 1; }
    printf("LIT=%u\nHOOK=%u\n", lit_pixels(), s_hook_calls);
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


def _run(exe: Path, mode: str, gpu: bool = False) -> subprocess.CompletedProcess:
    env = dict(os.environ)
    env["SR_GESTAT"] = "1"
    for name in ("SR_RTRACE", "SR_GE_TRANSITION_TRACE", "SR_NAN_TRAP", "SR_PIXWHO"):
        env.pop(name, None)
    argv = [os.fspath(exe), mode] + (["hook"] if gpu else [])
    return subprocess.run(argv, capture_output=True, text=True, env=env)


def _out(result: subprocess.CompletedProcess, key: str) -> int:
    match = re.search(rf"{key}=(\d+)", result.stdout)
    if not match:
        raise AssertionError(f"harness printed no {key}= line:\n{result.stdout}")
    return int(match.group(1))


def _stat(stderr: str, frame: int) -> dict:
    """Every counter the GESTAT window printed for `frame` (GESTAT and GESTAT+ merged)."""
    stat: dict[str, str] = {}
    for prefix in ("GESTAT+", "GESTAT"):
        rows = dict(re.findall(rf"{re.escape(prefix)} f=(\d+) (.*)", stderr))
        line = rows.get(str(frame))
        if line:
            stat.update(dict(re.findall(r"(\w+)=(\d+)", line)))
    return stat


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

    def test_through_nan_triangle_never_reaches_the_gpu_path(self):
        # THROUGH mode reads raw screen coordinates straight out of guest VRAM. Before the
        # fix nothing finiteness-checked them, so the primitive reached BOTH rasterizers -
        # and on Vulkan a NaN gl_Position has an undefined fixed-function clipper verdict.
        sw = _run(self.exe, "throughtri")
        self.assertEqual(sw.returncode, 0, sw.stdout + sw.stderr)
        self.assertEqual(_out(sw, "LIT"), 0,
                         "a NaN through-mode position must not rasterize in software")
        self.assertEqual(_out(sw, "HOOK"), 0, "no backend was registered")
        self.assertEqual(_stat(sw.stderr, 120).get("nonfinite"), "1")
        gpu = _run(self.exe, "throughtri", gpu=True)
        self.assertEqual(gpu.returncode, 0, gpu.stdout + gpu.stderr)
        self.assertEqual(_out(gpu, "HOOK"), 0,
                         "the Vulkan capture seam was handed a non-finite primitive")
        self.assertEqual(_out(gpu, "LIT"), 0,
                         "a taken primitive must not also be rasterized in software")

    def test_through_nan_sprite_never_reaches_the_gpu_path(self):
        sw = _run(self.exe, "throughspr")
        self.assertEqual(sw.returncode, 0, sw.stdout + sw.stderr)
        self.assertEqual(_out(sw, "LIT"), 0,
                         "a NaN sprite corner must not rasterize in software")
        gpu = _run(self.exe, "throughspr", gpu=True)
        self.assertEqual(gpu.returncode, 0, gpu.stdout + gpu.stderr)
        self.assertEqual(_out(gpu, "HOOK"), 0,
                         "the Vulkan capture seam was handed a non-finite sprite")

    def test_nan_patch_never_reaches_the_gpu_path(self):
        # Patch/spline tessellation submits through submit_model_triangle, a path with no
        # CVtx::nf verdict of its own to consult.
        sw = _run(self.exe, "patchnan")
        self.assertEqual(sw.returncode, 0, sw.stdout + sw.stderr)
        self.assertEqual(_out(sw, "LIT"), 0,
                         "a NaN bone matrix must not rasterize a patch in software")
        self.assertGreaterEqual(int(_stat(sw.stderr, 120).get("nonfinite", "0")), 1,
                                "the dropped patch triangle must be counted")
        gpu = _run(self.exe, "patchnan", gpu=True)
        self.assertEqual(gpu.returncode, 0, gpu.stdout + gpu.stderr)
        self.assertEqual(_out(gpu, "HOOK"), 0,
                         "the Vulkan capture seam was handed a non-finite patch triangle")

    def test_both_ge_paths_agree_on_the_finite_control(self):
        # The control must still be submitted to the GPU seam AND rasterize in software:
        # the fail-closed rule must not swallow ordinary geometry.
        for mode, kind in (("throughok", "triangle"), ("finite", "triangle")):
            with self.subTest(mode=mode):
                sw = _run(self.exe, mode)
                self.assertEqual(sw.returncode, 0, sw.stdout + sw.stderr)
                self.assertGreater(_out(sw, "LIT"), 0,
                                   f"control: a finite {kind} must still rasterize")
                self.assertEqual(_stat(sw.stderr, 120).get("nonfinite"), "0")
                gpu = _run(self.exe, mode, gpu=True)
                self.assertEqual(gpu.returncode, 0, gpu.stdout + gpu.stderr)
                self.assertEqual(_out(gpu, "HOOK"), 1,
                                 f"control: a finite {kind} must reach the GPU seam")
                self.assertEqual(_out(gpu, "LIT"), 0)
                self.assertEqual(_stat(gpu.stderr, 120).get("gpuprim"), "1",
                                 "a GPU-taken primitive must be counted for GESTAT")

    def test_gestat_reports_gpu_taken_primitives(self):
        # Without gpuprim a GPU-rasterized scene reports zero 3D primitives, because a
        # taken primitive returns before the tri2d/tri3d/spr/px counters.
        gpu = _run(self.exe, "throughok", gpu=True)
        stat = _stat(gpu.stderr, 120)
        self.assertEqual(stat.get("tri3d"), "0", "control: nothing was rasterized in software")
        self.assertEqual(stat.get("gpuprim"), "1")
        sw = _stat(_run(self.exe, "throughok").stderr, 120)
        self.assertEqual(sw.get("gpuprim"), "0", "no backend, nothing taken")

    def test_shared_predicate_verdicts(self):
        result = _run(self.exe, "predicate")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        rows = re.findall(r"PRED (\w+) (\d+) want=(\d+)", result.stdout)
        self.assertEqual(len(rows), 5, result.stdout)
        for name, got, want in rows:
            self.assertEqual(got, want, f"ge_vtx_finite verdict for {name}")

    def test_vulkan_path_applies_the_shared_rule(self):
        # ge_gpu.c cannot be compiled where Vulkan/SDL3 headers are absent, so pin the
        # contract in source: the Vulkan hooks must call the same predicate, and ge.c must
        # gate every rasterizer entry BEFORE it offers the primitive to the backend.
        gpu_src = (RT / "gpu_sdl3vk" / "ge_gpu.c").read_text(encoding="utf-8")
        self.assertIn("ge_vtx_finite(", gpu_src,
                      "the Vulkan hooks must test the shared rule")
        self.assertIn("static int hook_finite(", gpu_src)
        self.assertIn('"tris=%lu spr=%lu lines=%lu nonfinite=%lu', gpu_src,
                      "the Vulkan drop must be counted in the GEGPU stats line")
        for hook in ("tri", "sprite", "line", "point"):
            self.assertRegex(gpu_src, rf"(?s)static int hook_{hook}\(.{{0,400}}?hook_finite\(vv",
                             f"hook_{hook} must drop a non-finite primitive first")
        ge_src = (RT / "ge.c").read_text(encoding="utf-8")
        self.assertIn("static int vtx_nonfinite(", ge_src)
        for entry in ("raster_tri", "draw_line_vtx", "draw_point_vtx", "fill_sprite"):
            body = ge_src.split(f"static void {entry}(", 1)[1]
            gate = body.find("vtx_nonfinite(")
            seam = body.find("s_gpu->")
            self.assertGreaterEqual(gate, 0, f"{entry} must fail closed on a non-finite vertex")
            self.assertLess(gate, seam,
                            f"{entry} must fail closed BEFORE the GPU capture seam")

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
