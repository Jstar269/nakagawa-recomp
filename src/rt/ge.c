// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
//
/*
 * Walks a PSP GE command list and maintains the rendering state, then rasterises PRIM commands
 * into the framebuffer in guest VRAM.
 *
 * Through-mode (2D, screen-space UI/HUD) and transform-mode (3D, full T&L pipeline) are both
 * handled. Transform-mode does: world*view*proj matrix multiply, perspective divide, viewport
 * transform, per-vertex lighting (ambient/diffuse/specular, directional+point lights), fog,
 * depth test, perspective-correct UV interpolation with TEXSCALE/TEXOFFSET, and flat/gouraud
 * shading. Points, lines, triangles (list/strip/fan) and sprites are rasterised.
 *
 * Per-fragment: texturing (5650/5551/4444/8888/CLUT4/8/16/32/DXT1/3/5, swizzled or linear,
 * bilinear; DXT is inherently block-tiled and never swizzled),
 * programmable alpha test, colour test, alpha blending (full factor/equation set), stencil
 * test/ops (PSP stencil lives in destination alpha), logic ops, 4x4 ordered dithering,
 * colour/alpha write masks, scissor, and a software 16-bit depth buffer.
 *
 * Geometry: multi-weight vertex skinning (up to 8 bone matrices, decode_model_vtx) and
 * morph blending (up to 8 targets, read_pos/read_tc/read_nrm/read_color), plus Bezier and
 * B-spline patch primitives tessellated by the active PATCHDIVISION factors (draw_patch).
 *
 * Not emulated: mipmaps (level 0 only), the BJUMP bounding-box test.
 */

/*
 * Each GE command word is (cmd<<24)|data24. Command numbers follow PPSSPP GECommands.h.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"
#include "ge_shared.h"
#include "ge_capture.h"

#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

extern uint64_t SDL_GetTicksNS(void);

/* Keep fixture tracking local to the GE implementation: generated guest code and unrelated
 * HLE memory accesses retain their normal zero-overhead accessors. */
static uint8_t ge_capture_r8(uint32_t addr) { ge_capture_note_memory(addr, 1); return sr_r8(addr); }
static uint16_t ge_capture_r16(uint32_t addr) { ge_capture_note_memory(addr, 2); return sr_r16(addr); }
static uint32_t ge_capture_r32(uint32_t addr) { ge_capture_note_memory(addr, 4); return sr_r32(addr); }
static void ge_capture_w16(uint32_t addr, uint16_t value) { ge_capture_note_memory(addr, 2); sr_w16(addr, value); }
static void ge_capture_w32(uint32_t addr, uint32_t value) { ge_capture_note_memory(addr, 4); sr_w32(addr, value); }

#undef MEM_R8
#undef MEM_R16
#undef MEM_R32
#undef MEM_W16
#undef MEM_W32
#define MEM_R8(a) (g_ge_capture_active ? ge_capture_r8((a)) : sr_r8((a)))
#define MEM_R16(a) (g_ge_capture_active ? ge_capture_r16((a)) : sr_r16((a)))
#define MEM_R32(a) (g_ge_capture_active ? ge_capture_r32((a)) : sr_r32((a)))
#define MEM_W16(a, v) (g_ge_capture_active ? ge_capture_w16((a), (uint16_t)(v)) : sr_w16((a), (uint16_t)(v)))
#define MEM_W32(a, v) (g_ge_capture_active ? ge_capture_w32((a), (uint32_t)(v)) : sr_w32((a), (uint32_t)(v)))

/* Wall-clock ms (MinGW clock() is wall time with CLOCKS_PER_SEC=1000). */
static unsigned long wall_ms(void) { return (unsigned long)(clock() * 1000ull / CLOCKS_PER_SEC); }

/* ---- command numbers (subset) ---- */
enum {
    GE_NOP = 0x00, GE_VADDR = 0x01, GE_IADDR = 0x02, GE_PRIM = 0x04,
    GE_BEZIER = 0x05, GE_SPLINE = 0x06, GE_BOUNDINGBOX = 0x07,
    GE_JUMP = 0x08, GE_BJUMP = 0x09, GE_CALL = 0x0A, GE_RET = 0x0B,
    GE_END = 0x0C, GE_FINISH = 0x0F, GE_SIGNAL = 0x0E,
    GE_VERTEXTYPE = 0x12,
    GE_BASE = 0x10, GE_OFFSETADDR = 0x13, GE_ORIGIN = 0x14,
    GE_REGION1 = 0x15, GE_REGION2 = 0x16,
    GE_LIGHTINGENABLE = 0x17, GE_LIGHTENABLE0 = 0x18,
    GE_CLIPENABLE = 0x1C, GE_CULLFACEENABLE = 0x1D, GE_TEXTUREMAPENABLE = 0x1E,
    GE_FOGENABLE = 0x1F, GE_DITHERENABLE = 0x20, GE_ALPHABLENDENABLE = 0x21,
    GE_ALPHATESTENABLE = 0x22, GE_ZTESTENABLE = 0x23, GE_STENCILTESTENABLE = 0x24,
    GE_ANTIALIASENABLE = 0x25, GE_PATCHCULLENABLE = 0x26,
    GE_COLORTESTENABLE = 0x27, GE_LOGICOPENABLE = 0x28,
    GE_MORPHWEIGHT0 = 0x2C,
    GE_PATCHDIVISION = 0x36, GE_PATCHPRIMITIVE = 0x37, GE_PATCHFACING = 0x38,
    GE_WORLDMATRIXNUMBER = 0x3A, GE_WORLDMATRIXDATA = 0x3B,
    GE_VIEWMATRIXNUMBER  = 0x3C, GE_VIEWMATRIXDATA  = 0x3D,
    GE_PROJMATRIXNUMBER  = 0x3E, GE_PROJMATRIXDATA  = 0x3F,
    GE_TGENMATRIXNUMBER = 0x40, GE_TGENMATRIXDATA = 0x41,
    GE_VIEWPORTXSCALE = 0x42, GE_VIEWPORTYSCALE = 0x43, GE_VIEWPORTZSCALE = 0x44,
    GE_VIEWPORTXCENTER = 0x45, GE_VIEWPORTYCENTER = 0x46, GE_VIEWPORTZCENTER = 0x47,
    GE_TEXSCALEU = 0x48, GE_TEXSCALEV = 0x49, GE_TEXOFFSETU = 0x4A, GE_TEXOFFSETV = 0x4B,
    GE_OFFSETX = 0x4C, GE_OFFSETY = 0x4D,
    GE_SHADEMODE = 0x50, GE_REVERSENORMAL = 0x51, GE_MATERIALUPDATE = 0x53,
    GE_MATERIALEMISSIVE = 0x54, GE_MATERIALAMBIENT = 0x55, GE_MATERIALDIFFUSE = 0x56,
    GE_MATERIALSPECULAR = 0x57, GE_MATERIALALPHA = 0x58, GE_MATERIALSPECULARCOEF = 0x5B,
    GE_AMBIENTCOLOR = 0x5C, GE_AMBIENTALPHA = 0x5D, GE_LIGHTMODE = 0x5E,
    GE_LIGHTTYPE0 = 0x5F,                       /* ..0x62 */
    GE_LX0 = 0x63, GE_LDX0 = 0x6F, GE_LKA0 = 0x7B,   /* pos / dir / attenuation, 12 each */
    GE_LKS0 = 0x87, GE_LKO0 = 0x8B,                  /* spot exponent / cutoff, 4 each */
    GE_LAC0 = 0x8F,                                  /* light colours amb/diff/spec ×4 = 12 */
    GE_CULL = 0x9B,
    GE_FRAMEBUFPTR = 0x9C, GE_FRAMEBUFWIDTH = 0x9D, GE_ZBUFPTR = 0x9E, GE_ZBUFWIDTH = 0x9F,
    GE_TEXADDR0 = 0xA0, GE_TEXBUFWIDTH0 = 0xA8,
    GE_CLUTADDR = 0xB0, GE_CLUTADDRUPPER = 0xB1,
    GE_TEXSIZE0 = 0xB8,
    GE_TEXMAPMODE = 0xC0, GE_TEXSHADELS = 0xC1, GE_TEXMODE = 0xC2, GE_TEXFORMAT = 0xC3,
    GE_LOADCLUT = 0xC4, GE_CLUTFORMAT = 0xC5, GE_TEXFILTER = 0xC6, GE_TEXWRAP = 0xC7,
    GE_TEXLEVEL = 0xC8, GE_TEXFUNC = 0xC9, GE_TEXENVCOLOR = 0xCA,
    GE_TEXFLUSH = 0xCB, GE_TEXSYNC = 0xCC,
    GE_FOG1 = 0xCD, GE_FOG2 = 0xCE, GE_FOGCOLOR = 0xCF, GE_TEXLODSLOPE = 0xD0,
    GE_FRAMEBUFPIXFORMAT = 0xD2, GE_CLEARMODE = 0xD3,
    GE_SCISSOR1 = 0xD4, GE_SCISSOR2 = 0xD5,
    GE_MINZ = 0xD6, GE_MAXZ = 0xD7,
    GE_COLORTEST = 0xD8, GE_COLORREF = 0xD9, GE_COLORTESTMASK = 0xDA,
    GE_ALPHATEST = 0xDB, GE_STENCILTEST = 0xDC, GE_STENCILOP = 0xDD, GE_ZTEST = 0xDE,
    GE_BLENDMODE = 0xDF, GE_BLENDFIXEDA = 0xE0, GE_BLENDFIXEDB = 0xE1,
    GE_DITH0 = 0xE2, GE_DITH1 = 0xE3, GE_DITH2 = 0xE4, GE_DITH3 = 0xE5,
    GE_LOGICOP = 0xE6, GE_ZWRITEDISABLE = 0xE7,
    GE_MASKRGB = 0xE8, GE_MASKALPHA = 0xE9,
    GE_TRANSFERSRC = 0xB2, GE_TRANSFERSRCW = 0xB3,
    GE_TRANSFERDST = 0xB4, GE_TRANSFERDSTW = 0xB5,
    GE_TRANSFERSTART = 0xEA, GE_TRANSFERSRCPOS = 0xEB,
    GE_TRANSFERDSTPOS = 0xEC, GE_TRANSFERSIZE = 0xEE,
};

/* GeState now lives in ge_shared.h (shared with the optional GPU backend). */
static GeState ge;
static int s_ge_inited = 0;
static GeCpuProfileStats s_cpu_profile_stats;
static int s_cpu_profile = -1;
static int s_primitive_profile = -1;
static int s_primitive_profile_calibration;
static uint64_t s_primitive_profile_vertex_counter;
static uint64_t s_primitive_profile_triangle_counter;
static int s_primitive_profile_counting;
static int s_primitive_profile_through;
static int s_primitive_profile_pending_phase = -1;
static uint64_t s_primitive_profile_pending_started;
static uint64_t s_primitive_profile_total_pending_started;
static int s_primitive_profile_current_phase = -1;

static uint64_t primitive_profile_timer_overhead(void) {
    enum { CALIBRATION_PAIRS = 256 };
    volatile uint64_t sink = 0;
    uint64_t total = 0;
    for (int i = 0; i < CALIBRATION_PAIRS; i++) {
        uint64_t begin = SDL_GetTicksNS();
        uint64_t end = SDL_GetTicksNS();
        sink += end - begin;
        total += end - begin;
    }
    (void)sink;
    return total / CALIBRATION_PAIRS;
}

static void primitive_profile_configure(void) {
    if (s_primitive_profile >= 0) return;
    const char *value = getenv("SR_GE_PRIM_PROFILE");
    s_primitive_profile = value && value[0] && strcmp(value, "0") != 0;
    s_cpu_profile_stats.primitive_profile_enabled = s_primitive_profile;
    if (!s_primitive_profile) return;

    const char *calibration = getenv("SR_GE_PRIM_CALIBRATION");
    s_primitive_profile_calibration = calibration && calibration[0] && strcmp(calibration, "0") != 0;
    s_cpu_profile_stats.primitive_profile_calibration_enabled = s_primitive_profile_calibration;

    uint32_t stride = 512u;
    const char *stride_value = getenv("SR_GE_PRIM_PROFILE_STRIDE");
    if (stride_value && stride_value[0]) {
        char *end = NULL;
        unsigned long parsed = strtoul(stride_value, &end, 10);
        if (end != stride_value && *end == '\0' && parsed >= 1ul && parsed <= (1ul << 20))
            stride = (uint32_t)parsed;
    }
    s_cpu_profile_stats.primitive_profile_stride = stride;
    s_cpu_profile_stats.primitive_profile_timer_pair_ns = primitive_profile_timer_overhead();
}

static void ge_cpu_profile_configure(void) {
    if (s_cpu_profile >= 0) return;
    const char *ge_value = getenv("SR_GE_CPU_PROFILE");
    const char *gpu_value = getenv("SR_GPU_CPU_PROFILE");
    s_cpu_profile = ((ge_value && ge_value[0] && strcmp(ge_value, "0") != 0) ||
                     (gpu_value && gpu_value[0] && strcmp(gpu_value, "0") != 0));
    s_cpu_profile_stats.enabled = s_cpu_profile;
    primitive_profile_configure();
}

static uint64_t ge_cpu_profile_begin(void) {
    ge_cpu_profile_configure();
    return s_cpu_profile ? SDL_GetTicksNS() : 0;
}

static void ge_cpu_profile_end(GeCpuPhase phase, uint64_t started) {
    if (!s_cpu_profile) return;
    s_cpu_profile_stats.phase[phase].calls++;
    s_cpu_profile_stats.phase[phase].ns += SDL_GetTicksNS() - started;
}

static void primitive_profile_end_stats(GeCpuPhaseStats *stats, uint64_t started) {
    if (!s_primitive_profile || !stats || !started) return;
    uint64_t elapsed = SDL_GetTicksNS() - started;
    uint64_t overhead = s_cpu_profile_stats.primitive_profile_timer_pair_ns;
    if (elapsed > overhead) elapsed -= overhead;
    else elapsed = 0;
    stats->calls++;
    stats->ns += elapsed;
}

/* Keep the empty-control interval raw: unlike frontend phase estimates, its
 * purpose is to include the timestamp pair itself. */
static void primitive_profile_end_control(uint64_t started) {
    if (!s_primitive_profile || !started) return;
    GeCpuPhaseStats *stats = &s_cpu_profile_stats.primitive_profile_empty_control;
    stats->calls++;
    stats->ns += SDL_GetTicksNS() - started;
}

static void primitive_profile_end(GePrimitiveProfilePhase phase, uint64_t started) {
    primitive_profile_end_stats(&s_cpu_profile_stats.primitive_profile_phase[phase], started);
}

/* The sampled-total timer starts after the sparse claim and ends before the
 * raster/GPU hook, so it is one non-overlapping outer frontend interval. */
static void primitive_profile_finish_total(void) {
    if (!s_primitive_profile_calibration || !s_primitive_profile_total_pending_started) return;
    primitive_profile_end_stats(&s_cpu_profile_stats.primitive_profile_sampled_total,
                                s_primitive_profile_total_pending_started);
    s_primitive_profile_total_pending_started = 0;
}

/* Claim one sparse vertex event.  The first three sparse slots rotate through
 * fetch/decode, transform and lighting, so no phase gets a timer on every
 * vertex.  A through-mode vertex has only the fetch/decode phase. */
static int primitive_profile_claim_vertex(void) {
    if (!s_primitive_profile || !s_primitive_profile_counting) return -1;
    uint64_t ordinal = s_primitive_profile_vertex_counter++;
    uint32_t stride = s_cpu_profile_stats.primitive_profile_stride;
    if (!stride || (ordinal % stride) != 0) return -1;
    /* Empty control: selection plus the same timestamp/stat accounting, with
     * no vertex or triangle frontend work in the interval. */
    uint64_t control_started = s_primitive_profile_calibration ? SDL_GetTicksNS() : 0;
    int slot = (int)((ordinal / stride) % 3u);
    int phase;
    if (s_primitive_profile_through)
        phase = slot == 0 ? GE_PRIM_PROFILE_VERTEX_FETCH_DECODE : -1;
    else if (slot == 0) phase = GE_PRIM_PROFILE_VERTEX_FETCH_DECODE;
    else if (slot == 1) phase = GE_PRIM_PROFILE_TRANSFORM;
    else phase = GE_PRIM_PROFILE_LIGHTING;
    if (control_started)
        primitive_profile_end_control(control_started);
    return phase;
}

/* Return a rotating sparse slot for a triangle candidate.  Slot zero is
 * clipping/acceptance and slot one is post-acceptance assembly. */
static int primitive_profile_claim_triangle(void) {
    if (!s_primitive_profile || !s_primitive_profile_counting) return -1;
    uint64_t ordinal = s_primitive_profile_triangle_counter++;
    uint32_t stride = s_cpu_profile_stats.primitive_profile_stride;
    if (!stride || (ordinal % stride) != 0) return -1;
    uint64_t control_started = s_primitive_profile_calibration ? SDL_GetTicksNS() : 0;
    int slot = (int)((ordinal / stride) & 1u);
    if (control_started) {
        primitive_profile_end_control(control_started);
        s_primitive_profile_total_pending_started = SDL_GetTicksNS();
    }
    return slot;
}

static void primitive_profile_finish_fetch(void) {
    if (s_primitive_profile_pending_phase != GE_PRIM_PROFILE_VERTEX_FETCH_DECODE) return;
    primitive_profile_end(GE_PRIM_PROFILE_VERTEX_FETCH_DECODE,
                          s_primitive_profile_pending_started);
    s_primitive_profile_pending_phase = -1;
    s_primitive_profile_pending_started = 0;
}

static void primitive_profile_clear_vertex_pending(void) {
    s_primitive_profile_pending_phase = -1;
    s_primitive_profile_pending_started = 0;
}

static void primitive_profile_clear_pending(void) {
    primitive_profile_clear_vertex_pending();
    s_primitive_profile_total_pending_started = 0;
}

void ge_cpu_profile_reset(void) {
    s_cpu_profile = -1;
    s_primitive_profile = -1;
    s_primitive_profile_calibration = 0;
    s_primitive_profile_vertex_counter = 0;
    s_primitive_profile_triangle_counter = 0;
    s_primitive_profile_counting = 0;
    s_primitive_profile_through = 0;
    s_primitive_profile_pending_phase = -1;
    s_primitive_profile_pending_started = 0;
    s_primitive_profile_total_pending_started = 0;
    s_primitive_profile_current_phase = -1;
    memset(&s_cpu_profile_stats, 0, sizeof(s_cpu_profile_stats));
    ge_cpu_profile_configure();
}

void ge_cpu_profile_get(GeCpuProfileStats *out) {
    if (out) *out = s_cpu_profile_stats;
}

uint32_t g_ge_stall_addr = 0;
static uint32_t s_stack_pc[32], s_stack_offset[32];
static int s_sp = 0;

/* Optional GPU rasterizer hooks (ge_shared.h). NULL = pure software (the GDI build
 * never registers any; the SDL3 build registers them only under SR_GPU_GE=1). */
static const GeGpuHooks *s_gpu = NULL;
void ge_set_gpu_hooks(const GeGpuHooks *h) { s_gpu = h; }
GeState *ge_state_ptr(void) { return &ge; }
void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes) {
    if (s_gpu && s_gpu->vram_dirty) s_gpu->vram_dirty(addr, bytes);
}

/* Decode a GE 24-bit float mantissa word to IEEE-754 float.
 * The GE stores floats as the upper 24 bits of a 32-bit float (mantissa only; exp+sign). */
static float decode_float24(uint32_t data) {
    uint32_t f = data << 8;
    float r; memcpy(&r, &f, 4); return r;
}

static void ge_state_init(void) {
    ge.scis_x2 = 479; ge.scis_y2 = 271;
    ge.maxz = 0xFFFF;
    ge.tex_scale_u = ge.tex_scale_v = 1.0f;
    ge.shade_gouraud = 1;
    ge.amb_alpha = 0xFF;   /* lit alpha multiplies by this; 0 would blank lit geometry pre-init */
    ge.morph_weight[0] = 1.0f;
    s_ge_inited = 1;
}

/* ---- Depth buffer ----
 * Software 16-bit depth buffer, one entry per pixel, max stride 512.
 * Only active when ge.ztest_enable is set. */
static uint16_t s_zbuf[272 * 512];

static uint32_t zbuf_stride(void) { return ge.zbw ? ge.zbw : (ge.fbw ? ge.fbw : 512); }

uint16_t *ge_zbuf_ptr(void) { return s_zbuf; }
uint32_t  ge_zbuf_stride(void) { return zbuf_stride(); }

void ge_replay_restore(const GeState *state, const uint16_t *zbuf) {
    if (!state || !zbuf) return;
    ge = *state;
    memcpy(s_zbuf, zbuf, sizeof(s_zbuf));
    s_ge_inited = 1;
    s_sp = 0;
    g_ge_stall_addr = 0;
}

static void zbuf_fill_rect(int x0, int y0, int x1, int y1, uint16_t z) {
    uint32_t stride = zbuf_stride();
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    for (int y = y0; y <= y1; y++) {
        uint32_t base = (uint32_t)y * stride;
        for (int x = x0; x <= x1; x++) {
            uint32_t idx = base + (uint32_t)x;
            if (idx < 272u * 512u) s_zbuf[idx] = z;
        }
    }
}

static void stat_zfail(void);
static int ztest_compare_px(int x, int y, uint16_t z) {
    uint32_t idx = (uint32_t)(y * (int)zbuf_stride() + x);
    if (idx >= 272u * 512u) return 0;
    uint16_t cur = s_zbuf[idx];
    int pass;
    switch (ge.ztest & 7) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = (z == cur); break;
        case 3: pass = (z != cur); break;
        case 4: pass = (z < cur); break;
        case 5: pass = (z <= cur); break;
        case 6: pass = (z > cur); break;
        case 7: pass = (z >= cur); break;
        default: pass = 1;
    }
    if (!pass) stat_zfail();
    return pass;
}

/* Clamp interpolated depth to the 16-bit range. minz/maxz are NOT a clamp: the PSP
 * depth-range test DISCARDS transform-mode pixels outside [minz,maxz] (see zrange_ok). */
static uint16_t clamp_z(float zf) {
    if (zf < 0.0f) zf = 0.0f; else if (zf > 65535.0f) zf = 65535.0f;
    return (uint16_t)zf;
}

/* PSP depth-range test (PPSSPP DrawPixel.cpp DrawSinglePixel): every transform-mode pixel
 * with z outside [minz,maxz] is dropped — even in clear mode, even with z-test disabled.
 * Games use this to mask effect passes (e.g. the hangar reflection copy of the aircraft).
 * Clamping instead of discarding made those passes paint over the scene. */
static int zrange_ok(float zf, int persp) {
    if (!persp) return 1;
    int z = (int)zf;
    return z >= (int)ge.minz && z <= (int)ge.maxz;
}

/* Decode the vertex format. Supports 8/16/float texcoord, colour, normal, and position.
 * Returns the stride; fills offsets (-1 if absent). through=1 means screen-space (2D). */
typedef struct {
    int stride, one_stride, morph_n, tc_off, tc_fmt, col_off, col_fmt;
    int nrm_off, nrm_fmt;    /* normal (lighting; -1 if absent) */
    int pos_off, pos_fmt;
    int w_off, w_fmt, w_n;   /* skinning weights (w_n==0 if absent) */
    int through;
} VFmt;

static void decode_vtype(uint32_t vt, VFmt *v) {
    memset(v, 0, sizeof(*v));
    v->nrm_off = -1; v->tc_off = -1; v->col_off = -1;
    int tc = vt & 3, col = (vt >> 2) & 7, nrm = (vt >> 5) & 3, pos = (vt >> 7) & 3;
    int wt = (vt >> 9) & 3;   /* weight format (skinning) */
    int wc = (vt >> 14) & 7;  /* weight count */
    v->morph_n = (int)((vt >> 18) & 7) + 1;
    v->through = (vt >> 23) & 1;

    static const int csz[4] = { 0, 1, 2, 4 };               /* none/8-bit/16-bit/float */
    static const int colsz[8] = { 0, 0, 0, 0, 2, 2, 2, 4 }; /* 4=5650,5=5551,6=4444,7=8888 */
    int off = 0, maxa = 1;

    /* Skinning weights come first in the vertex if present. The count field is n-1: wc==0
     * with a nonzero format means ONE weight (the old `wt && wc` guard skipped that case,
     * misaligning every later field — the hangar aircraft uses single-weight vertices). */
    if (wt) {
        int a = csz[wt];
        off = (off + a - 1) & ~(a - 1);
        v->w_fmt = wt; v->w_n = wc + 1; v->w_off = off;
        off += a * (wc + 1);
        if (a > maxa) maxa = a;
    }

    v->tc_fmt = tc;
    if (tc) { int a = csz[tc]; off = (off+a-1)&~(a-1); v->tc_off = off; off += a*2; if (a>maxa) maxa=a; }

    v->col_fmt = col;
    if (col) { int a = colsz[col]; off = (off+a-1)&~(a-1); v->col_off = off; off += a; if (a>maxa) maxa=a; }

    v->nrm_fmt = nrm;
    if (nrm) { int a = csz[nrm]; off = (off+a-1)&~(a-1); v->nrm_off = off; off += a*3; if (a>maxa) maxa=a; }

    v->pos_fmt = pos;
    { int a = csz[pos ? pos : 3]; off = (off+a-1)&~(a-1); v->pos_off = off; off += a*3; if (a>maxa) maxa=a; }

    v->one_stride = (off + maxa - 1) & ~(maxa - 1);
    v->stride = v->one_stride * v->morph_n;
}

/* Read position from a vertex. For transform-mode, all components are signed (model space).
 * For through-mode, x and y are screen pixels; z is a 16-bit unsigned depth (0..65535). */
/* Transform-mode fixed-point positions are normalized like the hardware: s8/128, s16/32768
 * (PPSSPP VertexDecoder Step_PosS8ToFloat/Step_PosS16ToFloat). Through-mode keeps raw screen
 * units (x/y signed, z unsigned depth). Reading transform-mode s16 unscaled made every
 * fixed-point model (briefing map, hangar geometry) 32768x too large. */
static void read_pos_one(uint32_t addr, const VFmt *v, float *x, float *y, float *z) {
    uint32_t a = addr + (uint32_t)v->pos_off;
    if (v->pos_fmt == 3) {
        uint32_t xi = MEM_R32(a), yi = MEM_R32(a+4), zi = MEM_R32(a+8);
        memcpy(x, &xi, 4); memcpy(y, &yi, 4); memcpy(z, &zi, 4);
    } else if (v->pos_fmt == 2) {
        if (v->through) {
            *x = (float)(int16_t)MEM_R16(a);
            *y = (float)(int16_t)MEM_R16(a+2);
            *z = (float)(uint16_t)MEM_R16(a+4);
        } else {
            *x = (float)(int16_t)MEM_R16(a)   * (1.0f / 32768.0f);
            *y = (float)(int16_t)MEM_R16(a+2) * (1.0f / 32768.0f);
            *z = (float)(int16_t)MEM_R16(a+4) * (1.0f / 32768.0f);
        }
    } else {
        if (v->through) {
            *x = (float)(int8_t)MEM_R8(a);
            *y = (float)(int8_t)MEM_R8(a+1);
            *z = (float)(uint8_t)MEM_R8(a+2);
        } else {
            *x = (float)(int8_t)MEM_R8(a)   * (1.0f / 128.0f);
            *y = (float)(int8_t)MEM_R8(a+1) * (1.0f / 128.0f);
            *z = (float)(int8_t)MEM_R8(a+2) * (1.0f / 128.0f);
        }
    }
}

static void read_pos(uint32_t addr, const VFmt *v, float *x, float *y, float *z) {
    if (v->morph_n <= 1 || v->through) { read_pos_one(addr,v,x,y,z); return; }
    float ax=0.0f, ay=0.0f, az=0.0f;
    for (int i=0;i<v->morph_n;i++) {
        float tx,ty,tz,w=ge.morph_weight[i];
        read_pos_one(addr+(uint32_t)(i*v->one_stride),v,&tx,&ty,&tz);
        ax+=tx*w; ay+=ty*w; az+=tz*w;
    }
    *x=ax; *y=ay; *z=az;
}

/* Read texcoords. Through-mode: absolute texel coordinates. Transform-mode: UNSIGNED u8
 * scaled by 1/128 and UNSIGNED u16 by 1/32768 (PPSSPP Step_TcU8ToFloat/Step_TcU16ToFloat).
 * Reading these as signed flips UVs >= half range (used for texture tiling: 1.0..2.0)
 * to small negatives, which sample the wrong texels. */
static void read_tc_one(uint32_t addr, const VFmt *v, float *u, float *tv) {
    if (v->tc_off < 0) { *u = 0; *tv = 0; return; }
    uint32_t a = addr + (uint32_t)v->tc_off;
    if (v->tc_fmt == 3) {
        uint32_t ui = MEM_R32(a), vi = MEM_R32(a+4);
        memcpy(u, &ui, 4); memcpy(tv, &vi, 4);
    } else if (v->tc_fmt == 2) {
        if (v->through) {
            *u  = (float)(uint16_t)MEM_R16(a);
            *tv = (float)(uint16_t)MEM_R16(a+2);
        } else {
            *u  = (float)(uint16_t)MEM_R16(a)  * (1.0f / 32768.0f);
            *tv = (float)(uint16_t)MEM_R16(a+2) * (1.0f / 32768.0f);
        }
    } else {
        if (v->through) {
            *u  = (float)MEM_R8(a);
            *tv = (float)MEM_R8(a+1);
        } else {
            *u  = (float)MEM_R8(a)  * (1.0f / 128.0f);
            *tv = (float)MEM_R8(a+1) * (1.0f / 128.0f);
        }
    }
}

static void read_tc(uint32_t addr, const VFmt *v, float *u, float *tv) {
    if (v->tc_off < 0) { *u=0.0f; *tv=0.0f; return; }
    if (v->morph_n <= 1 || v->through) { read_tc_one(addr,v,u,tv); return; }
    float au=0.0f,av=0.0f;
    for (int i=0;i<v->morph_n;i++) {
        float tu,tv1,w=ge.morph_weight[i];
        read_tc_one(addr+(uint32_t)(i*v->one_stride),v,&tu,&tv1);
        au+=tu*w; av+=tv1*w;
    }
    *u=au; *tv=av;
}

/* Read a vertex normal (model space). Defaults to +Z when absent. */
static void read_nrm_one(uint32_t addr, const VFmt *v, float *x, float *y, float *z) {
    if (v->nrm_off < 0) { *x = 0; *y = 0; *z = 1; return; }
    uint32_t a = addr + (uint32_t)v->nrm_off;
    if (v->nrm_fmt == 3) {
        uint32_t xi = MEM_R32(a), yi = MEM_R32(a+4), zi = MEM_R32(a+8);
        memcpy(x, &xi, 4); memcpy(y, &yi, 4); memcpy(z, &zi, 4);
    } else if (v->nrm_fmt == 2) {
        *x = (float)(int16_t)MEM_R16(a)   * (1.0f / 32768.0f);
        *y = (float)(int16_t)MEM_R16(a+2) * (1.0f / 32768.0f);
        *z = (float)(int16_t)MEM_R16(a+4) * (1.0f / 32768.0f);
    } else {
        *x = (float)(int8_t)MEM_R8(a)   * (1.0f / 128.0f);
        *y = (float)(int8_t)MEM_R8(a+1) * (1.0f / 128.0f);
        *z = (float)(int8_t)MEM_R8(a+2) * (1.0f / 128.0f);
    }
}

static void read_nrm(uint32_t addr, const VFmt *v, float *x, float *y, float *z) {
    if (v->nrm_off < 0 || v->morph_n <= 1 || v->through) {
        read_nrm_one(addr,v,x,y,z);
    } else {
        float ax=0.0f,ay=0.0f,az=0.0f;
        for (int i=0;i<v->morph_n;i++) {
            float tx,ty,tz,w=ge.morph_weight[i];
            read_nrm_one(addr+(uint32_t)(i*v->one_stride),v,&tx,&ty,&tz);
            ax+=tx*w; ay+=ty*w; az+=tz*w;
        }
        *x=ax; *y=ay; *z=az;
    }
    if (ge.reverse_normal) { *x=-*x; *y=-*y; *z=-*z; }
}

/* Skinning weights: unsigned fixed-point, u8/128 and u16/32768 (PPSSPP VertexDecoder
 * Step_WeightsU8/U16), float raw. */
static void read_weights(uint32_t addr, const VFmt *v, float *w) {
    uint32_t a = addr + (uint32_t)v->w_off;
    for (int i = 0; i < v->w_n; i++) {
        if (v->w_fmt == 3) { uint32_t t = MEM_R32(a + 4u*(uint32_t)i); memcpy(&w[i], &t, 4); }
        else if (v->w_fmt == 2) w[i] = (float)MEM_R16(a + 2u*(uint32_t)i) * (1.0f / 32768.0f);
        else                    w[i] = (float)MEM_R8(a + (uint32_t)i)     * (1.0f / 128.0f);
    }
}

/* Convert a colour in PSP pixel format `psm` (0=5650,1=5551,2=4444,3=8888) read from `raw` into
 * 8-bit R,G,B,A. For 16-bit formats `raw` holds the 16-bit value; for 8888 the full 32 bits. */
static void unpack_color(uint32_t raw, int psm, int *r, int *g, int *b, int *a) {
    switch (psm & 3) {
        case 0: {
            int r5=(int)(raw&0x1F), g6=(int)((raw>>5)&0x3F), b5=(int)((raw>>11)&0x1F);
            *r=(r5<<3)|(r5>>2); *g=(g6<<2)|(g6>>4); *b=(b5<<3)|(b5>>2); *a=255; break;
        }
        case 1: {
            int r5=(int)(raw&0x1F), g5=(int)((raw>>5)&0x1F), b5=(int)((raw>>10)&0x1F);
            *r=(r5<<3)|(r5>>2); *g=(g5<<3)|(g5>>2); *b=(b5<<3)|(b5>>2); *a=(raw&0x8000)?255:0; break;
        }
        case 2: *r=(int)((raw&0xF)*17); *g=(int)(((raw>>4)&0xF)*17); *b=(int)(((raw>>8)&0xF)*17); *a=(int)(((raw>>12)&0xF)*17); break;
        default: *r=raw&0xFF; *g=(raw>>8)&0xFF; *b=(raw>>16)&0xFF; *a=(raw>>24)&0xFF; break;
    }
}

/* Read a vertex colour into 8-bit RGBA (col_fmt: 4=5650,5=5551,6=4444,7=8888). */
static void read_color_one(uint32_t addr, const VFmt *v, int *r, int *g, int *b, int *a) {
    if (v->col_off < 0) {
        if (ge.material_set || ge.material_alpha_set) {
            *r = ge.material_set ? (ge.material & 0xFF) : 255;
            *g = ge.material_set ? ((ge.material >> 8) & 0xFF) : 255;
            *b = ge.material_set ? ((ge.material >> 16) & 0xFF) : 255;
            *a = ge.material_alpha_set ? ge.material_alpha : 255;
        } else {
            *r=*g=*b=*a=255;
        }
        return;
    }
    uint32_t ca = addr + (uint32_t)v->col_off;
    if (v->col_fmt == 7) unpack_color(MEM_R32(ca), 3, r, g, b, a);
    else unpack_color(MEM_R16(ca), v->col_fmt - 4, r, g, b, a);
}

static void read_color(uint32_t addr, const VFmt *v, int *r, int *g, int *b, int *a) {
    if (v->col_off < 0 || v->morph_n <= 1 || v->through) {
        read_color_one(addr,v,r,g,b,a); return;
    }
    float c[4]={0,0,0,0};
    for (int i=0;i<v->morph_n;i++) {
        uint32_t ca=addr+(uint32_t)(i*v->one_stride+v->col_off);
        float w=ge.morph_weight[i], q[4];
        if (v->col_fmt==7) {
            uint32_t x=MEM_R32(ca);
            q[0]=(float)(x&255); q[1]=(float)((x>>8)&255);
            q[2]=(float)((x>>16)&255); q[3]=(float)((x>>24)&255);
        } else {
            uint32_t x=MEM_R16(ca);
            if (v->col_fmt==4) {
                q[0]=(float)(x&31)*(255.0f/31.0f); q[1]=(float)((x>>5)&63)*(255.0f/63.0f);
                q[2]=(float)((x>>11)&31)*(255.0f/31.0f); q[3]=255.0f;
            } else if (v->col_fmt==5) {
                q[0]=(float)(x&31)*(255.0f/31.0f); q[1]=(float)((x>>5)&31)*(255.0f/31.0f);
                q[2]=(float)((x>>10)&31)*(255.0f/31.0f); q[3]=(x&0x8000)?255.0f:0.0f;
            } else {
                q[0]=(float)(x&15)*17.0f; q[1]=(float)((x>>4)&15)*17.0f;
                q[2]=(float)((x>>8)&15)*17.0f; q[3]=(float)((x>>12)&15)*17.0f;
            }
        }
        for (int j=0;j<4;j++) c[j]+=q[j]*w;
    }
    int ci[4]={(int)c[0],(int)c[1],(int)c[2],(int)c[3]};
    for (int j=0;j<4;j++) if (ci[j]<0) ci[j]=0; else if (ci[j]>255) ci[j]=255;
    *r=ci[0]; *g=ci[1]; *b=ci[2]; *a=ci[3];
}

/* Look up a CLUT entry (palette index -> RGBA). */
static void clut_lookup(uint32_t index, int *r, int *g, int *b, int *a) {
    uint32_t shift = (ge.clut_fmt >> 2) & 0x1F;
    uint32_t mask  = (ge.clut_fmt >> 8) & 0xFF;
    uint32_t start = ((ge.clut_fmt >> 16) & 0x1F) << 4;
    uint32_t start_mask = (ge.clut_fmt & 3) == 3 ? 0xFFu : 0x1FFu;
    index = ((index >> shift) & mask) | (start & start_mask);
    int epsm = ge.clut_fmt & 3;
    if (epsm == 3) { uint32_t raw; memcpy(&raw, &ge.clutram[(index * 4) & 2047], 4); unpack_color(raw, 3, r, g, b, a); }
    else           { uint16_t raw; memcpy(&raw, &ge.clutram[(index * 2) & 2047], 2); unpack_color(raw, epsm, r, g, b, a); }
}

unsigned long g_tex_samples = 0, g_tex_nonzero = 0;
static uint32_t s_ge_frame = 0;
static int s_capture_configured = 0;
static uint32_t s_capture_frame = UINT32_MAX;
static uint32_t s_capture_frame_end = UINT32_MAX;
static unsigned long s_capture_min_prims = 1;
static unsigned long s_capture_total_prims = 0;
static int s_capture_done = 0;
static int s_capture_exit = 0;
static const char *s_capture_path = NULL;

static void ge_capture_configure(void) {
    if (s_capture_configured) return;
    s_capture_configured = 1;
    const char *frame_text = getenv("SR_GE_CAPTURE_FRAME");
    if (!frame_text || !frame_text[0]) return;
    char *frame_end = NULL;
    unsigned long value = strtoul(frame_text, &frame_end, 10);
    if (value > UINT32_MAX || frame_end == frame_text) return;
    s_capture_frame = (uint32_t)value;
    s_capture_frame_end = s_capture_frame;
    if (*frame_end == '-') {
        char *range_end = NULL;
        unsigned long end_value = strtoul(frame_end + 1, &range_end, 10);
        if (end_value > UINT32_MAX || range_end == frame_end + 1 || *range_end != '\0' ||
            end_value < value) {
            s_capture_frame = UINT32_MAX;
            s_capture_frame_end = UINT32_MAX;
            return;
        }
        s_capture_frame_end = (uint32_t)end_value;
    } else if (*frame_end != '\0') {
        s_capture_frame = UINT32_MAX;
        s_capture_frame_end = UINT32_MAX;
        return;
    }
    s_capture_path = getenv("SR_GE_CAPTURE_PATH");
    if (!s_capture_path || !s_capture_path[0]) s_capture_path = "logs/ge_capture.ngef";
    const char *min_text = getenv("SR_GE_CAPTURE_MIN_PRIMS");
    if (min_text && min_text[0]) s_capture_min_prims = strtoul(min_text, NULL, 10);
    const char *exit_text = getenv("SR_GE_CAPTURE_EXIT");
    s_capture_exit = exit_text && exit_text[0] && strcmp(exit_text, "0") != 0;
    if (s_capture_frame_end == s_capture_frame) {
        fprintf(stderr, "GE_CAPTURE: armed frame=%u min_prims=%lu path=%s\n",
                s_capture_frame, s_capture_min_prims, s_capture_path);
    } else {
        fprintf(stderr, "GE_CAPTURE: armed frames=%u-%u min_prims=%lu path=%s\n",
                s_capture_frame, s_capture_frame_end, s_capture_min_prims, s_capture_path);
    }
}

/* ---- Per-frame statistics (SR_GESTAT=1: print every 60 frames) ----
 * Used to diagnose "black screen" states: shows whether 3D geometry is being culled,
 * near-clipped, z-rejected, alpha-killed, or drawn. */
static struct {
    unsigned long tri2d, tri3d, spr2d, spr3d, cull, nearclip, zfail, afail, px2d, px3d, zrange;
    unsigned long mw_world, mw_view, mw_proj;   /* matrix data words captured */
    unsigned long blk3d;                        /* 3D pixels written with final RGB == 0 */
    uint32_t fbs[4]; unsigned long fbn[4];      /* distinct draw-buffer addrs hit + pixel counts */
    unsigned long fog0, fogfull, texa0;         /* full-fog pixels, no-fog pixels, alpha==0 texels */
    unsigned long skin, bonew;                  /* skinned (weighted) 3D prims, bone matrix writes */
    unsigned long deg3d;                        /* zero-area 3D triangles (zeroed/missing vertex data) */
    unsigned long line3d;                       /* 3D line segments submitted (wireframe map etc.) */
} s_stat;

/* SR_GEMATW=1: trace the first view/proj matrix register writes (cursor + decoded value)
 * to distinguish "uploads never arrive" from "uploads land in the wrong slots". */
static void trace_mtx_write(const char *name, int cursor, uint32_t data) {
    static int on = -1, n = 0;
    if (on < 0) on = getenv("SR_GEMATW") ? 1 : 0;
    if (!on || n >= 120) return;
    n++;
    fprintf(stderr, "GEMATW f=%u %s[%d] = 0x%06x (%g)\n",
            s_ge_frame, name, cursor, data, decode_float24(data));
}
static void ge_snapshot_plain(const char *path);   /* defined near ge_snapshot below */

static int s_stat_on = -1;
static unsigned long s_ge_ms_acc = 0;   /* wall ms spent inside ge_run_list this stat window */
static unsigned long s_wall_last = 0;   /* wall ms at end of previous stat window */
static int s_in3d = 0;   /* current raster path is transform-mode (for px2d/px3d split) */
static int s_ashade = 0; /* per-window budget for alpha-test failure dumps (SR_GESTAT) */
static int s_pixwho_n = 0; /* per-window budget for SR_PIXWHO pixel-ownership dumps */
static int s_pxpass = 0; /* per-window budget for passing-3D-pixel dumps (SR_GESTAT) */
static int s_pxc = 0;    /* per-window budget for centre-pixel write traces (SR_GESTAT) */
static int s_neardump = 0; /* per-window budget for fully-near-clipped primitive dumps (SR_GESTAT) */
static int s_ge3d = 0;     /* per-window budget for per-3D-draw GE3D lines (SR_GESTAT) */
static uint32_t s_ge3d_sigs[48];  /* distinct draw signatures already printed this window */

/* ---- SR_RTRACE=1: exhaustive render trace ----
 * For the first SR_RTRACE_FRAMES (default 2) transform-mode frames of every 60-frame stat
 * window, logs ONE line per 3D draw call with the complete GE state plus per-reason
 * accept/drop counters (TRIDRW/TRIDRW+), and one line per triangle with clip-space coords,
 * screen-grid coords and acceptance flags (TRIDEC) — capped per draw, EXCEPT triangles whose
 * screen bbox covers the SR_PIXWHO probe pixel, which always log (drawn or dropped). */
static int s_rtrace = -1;            /* env flag, lazy init */
static int s_rt_fbudget = 2;         /* frames traced per window (SR_RTRACE_FRAMES) */
static int s_rt_frames = 0;          /* frames traced so far this window */
static uint32_t s_rt_cur_frame = 0xFFFFFFFFu;
static int s_rt_active = 0;          /* tracing the current frame */
static uint32_t s_rt_draw = 0;       /* global 3D draw sequence number */
static int s_rt_draws_f = 0;         /* draws logged this frame (cap) */

/* Per-texture alpha-test outcome counters for transform-mode fragments (SR_GESTAT). Shows which
 * textures a scene's 3D actually samples and which of them the alpha test eats. */
#define ATEX_N 16
static struct { uint32_t addr; unsigned long fail, pass; } s_atex[ATEX_N];
static void atex_note(uint32_t addr, int pass) {
    for (int i = 0; i < ATEX_N; i++) {
        if (s_atex[i].addr == addr || (s_atex[i].fail | s_atex[i].pass) == 0) {
            s_atex[i].addr = addr;
            if (pass) s_atex[i].pass++; else s_atex[i].fail++;
            return;
        }
    }
}
static void stat_zfail(void) { s_stat.zfail++; }

/* Through-mode primitives honour the depth test like the hardware; SR_NO2DZ=1 disables
 * that for bisecting bad-depth-state symptoms. */
static int thru_ztest(void) {
    static int no2dz = -1;
    if (no2dz < 0) no2dz = getenv("SR_NO2DZ") ? 1 : 0;
    return ge.ztest_enable && !no2dz;
}

void ge_set_frame(uint32_t frame) {
    ge_capture_configure();
    if (ge_capture_active() && frame != s_ge_frame) {
        int boundary_ok = !s_gpu || !s_gpu->capture_boundary || s_gpu->capture_boundary();
        if (!boundary_ok) {
            fprintf(stderr, "GE_CAPTURE: final GPU boundary materialization failed\n");
            ge_capture_abort();
        } else if (s_capture_total_prims < s_capture_min_prims) {
            fprintf(stderr, "GE_CAPTURE: skipped frame=%u prims=%lu below minimum=%lu\n",
                    s_ge_frame, s_capture_total_prims, s_capture_min_prims);
            ge_capture_abort();
        } else {
            s_capture_done = ge_capture_end(&ge);
            if (s_capture_done && s_capture_exit) {
                fflush(NULL);
                _Exit(0);
            }
        }
    }
    s_ge_frame = frame;
    if (s_stat_on < 0) s_stat_on = getenv("SR_GESTAT") ? 1 : 0;
    if (s_stat_on && frame && (frame % 60) == 0) {
        unsigned long now = wall_ms();
        fprintf(stderr,
            "GESTAT f=%u wall=%lums ge=%lums tri2d=%lu tri3d=%lu spr2d=%lu spr3d=%lu cull=%lu near=%lu zfail=%lu afail=%lu px2d=%lu px3d=%lu mw=%lu/%lu/%lu\n",
            frame, now - s_wall_last, s_ge_ms_acc,
            s_stat.tri2d, s_stat.tri3d, s_stat.spr2d, s_stat.spr3d,
            s_stat.cull, s_stat.nearclip, s_stat.zfail, s_stat.afail, s_stat.px2d, s_stat.px3d,
            s_stat.mw_world, s_stat.mw_view, s_stat.mw_proj);
        s_wall_last = now;
        s_ge_ms_acc = 0;
        fprintf(stderr,
            "GESTAT+ f=%u blk3d=%lu skin=%lu bonew=%lu deg3d=%lu line3d=%lu zrange=%lu zr=[%u,%u] fmt=%u zbp=0x%08x fb=[0x%08x:%lu 0x%08x:%lu 0x%08x:%lu 0x%08x:%lu]\n",
            frame, s_stat.blk3d, s_stat.skin, s_stat.bonew, s_stat.deg3d, s_stat.line3d,
            s_stat.zrange, ge.minz, ge.maxz, ge.fbfmt, ge.zbp,
            s_stat.fbs[0], s_stat.fbn[0], s_stat.fbs[1], s_stat.fbn[1],
            s_stat.fbs[2], s_stat.fbn[2], s_stat.fbs[3], s_stat.fbn[3]);
        fprintf(stderr,
            "GESTAT* f=%u fog0=%lu fogfull=%lu texa0=%lu/%lu atest=%d/0x%06x fogc=0x%06x fend=%g fslope=%g vp=(%.4g,%.4g,%.4g,%.4g) off=(%.4g,%.4g)\n",
            frame, s_stat.fog0, s_stat.fogfull, s_stat.texa0, g_tex_samples,
            ge.atest_enable, ge.atest, ge.fog_color, ge.fog_end, ge.fog_slope,
            decode_float24(ge.viewport_raw[0]), decode_float24(ge.viewport_raw[3]),
            decode_float24(ge.viewport_raw[1]), decode_float24(ge.viewport_raw[4]),
            (float)(ge.offsetx & 0xFFFF) / 16.0f, (float)(ge.offsety & 0xFFFF) / 16.0f);
        for (int i = 0; i < ATEX_N; i++) {
            if (s_atex[i].fail + s_atex[i].pass < 500) continue;
            fprintf(stderr, "ATEX f=%u addr=0x%08x fail=%lu pass=%lu\n",
                    frame, s_atex[i].addr, s_atex[i].fail, s_atex[i].pass);
        }
        memset(s_atex, 0, sizeof(s_atex));
        s_ashade = 0;
        s_pxpass = 0;
        s_pxc = 0;
        s_neardump = 0;
        s_ge3d = 0;
        s_pixwho_n = 0;
        s_rt_frames = 0;
        memset(&s_stat, 0, sizeof(s_stat));
        /* SR_FBSNAP=1: dump the current GE framebuffer once per stat window (max 40 files,
         * or SR_FBSNAP=<n> for a custom cap) so misrendered scenes can be inspected
         * pixel-exactly alongside the trace. */
        {
            static int fbsnap = -1; static int nsnap = 0;
            if (fbsnap < 0) {
                const char *fs = getenv("SR_FBSNAP");
                fbsnap = 0;
                if (fs && fs[0]) { fbsnap = atoi(fs); if (fbsnap < 2) fbsnap = 40; }
            }
            if (fbsnap && nsnap < fbsnap) {
                char sp[96];
                snprintf(sp, sizeof(sp), "snap_f%05u.ppm", frame);
                ge_snapshot_plain(sp);
                nsnap++;
            }
        }
    }
}

static uint32_t texel_offset(uint32_t row_pitch_pixels, uint32_t u, uint32_t v, uint32_t texel_bits) {
    if (!ge.tex_swizzle)
        return v * ((row_pitch_pixels * texel_bits) >> 3) + ((u * texel_bits) >> 3);

    /* PPSSPP Software/Sampler.cpp GetPixelDataOffset() */
    const uint32_t tile_bits = 32, tiles_x = 4, tiles_y = 8;
    uint32_t texels_per_tile = tile_bits / texel_bits;
    uint32_t tile_u   = u / texels_per_tile;
    uint32_t tile_idx = (v % tiles_y) * tiles_x
        + (v / tiles_y) * ((row_pitch_pixels * texel_bits / tile_bits) * tiles_y)
        + (tile_u % tiles_x)
        + (tile_u / tiles_x) * (tiles_x * tiles_y);
    return tile_idx * (tile_bits >> 3) + (((u % texels_per_tile) * texel_bits) >> 3);
}

static int tex_coord(int v, int size, int clamp_mode) {
    if (size <= 0) return 0;
    if (clamp_mode) {
        if (v < 0) return 0;
        if (v >= size) return size - 1;
        return v;
    }
    return v & (size - 1) & 511;
}

/* ---- DXT block decode (PSP layout: 2-bit index lines FIRST, then the two 565 colors;
 * DXT3/5 append their alpha sub-block AFTER the color sub-block, the reverse of PC DXT).
 * Color endpoints are PSP 565 (r in the low bits) expanded by bit replication, and the
 * 1/3-2/3 mixes use the same integer formulas as PPSSPP's decoder, our reference.
 * DXT textures are never swizzled: the 4x4 block tiling IS the storage layout. */

static void dxt_color_block(uint32_t base, uint32_t x, uint32_t y, int fourcolor,
                            int *r, int *g, int *b, int *a) {
    /* base points at the 8-byte color sub-block: u8 lines[4]; u16 color1; u16 color2 */
    uint16_t c1 = MEM_R16(base + 4), c2 = MEM_R16(base + 6);
    int idx = (MEM_R8(base + y) >> (x * 2)) & 3;
    int r1, g1, b1, a1, r2, g2, b2, a2;
    unpack_color(c1, 0, &r1, &g1, &b1, &a1);
    unpack_color(c2, 0, &r2, &g2, &b2, &a2);
    *a = 255;
    switch (idx) {
        case 0: *r = r1; *g = g1; *b = b1; break;
        case 1: *r = r2; *g = g2; *b = b2; break;
        case 2:
            if (fourcolor || c1 > c2) { *r = (r1*2+r2)/3; *g = (g1*2+g2)/3; *b = (b1*2+b2)/3; }
            else                      { *r = (r1+r2)/2;   *g = (g1+g2)/2;   *b = (b1+b2)/2; }
            break;
        default:
            if (fourcolor || c1 > c2) { *r = (r1+r2*2)/3; *g = (g1+g2*2)/3; *b = (b1+b2*2)/3; }
            else                      { *r = 0; *g = 0; *b = 0; *a = 0; }   /* DXT1 punch-through */
            break;
    }
}

static void sample_dxt(uint32_t stride, uint32_t u, uint32_t v, int *r, int *g, int *b, int *a) {
    uint32_t bpr = (stride / 4 ? stride / 4 : 1);        /* blocks per row */
    uint32_t bsize = ge.tex_fmt == 8 ? 8u : 16u;
    uint32_t block = ge.tex_addr + ((v / 4) * bpr + (u / 4)) * bsize;
    uint32_t x = u & 3, y = v & 3;
    dxt_color_block(block, x, y, ge.tex_fmt != 8, r, g, b, a);
    if (ge.tex_fmt == 9) {           /* DXT3: 4-bit explicit alpha, u16 per row */
        int av = (MEM_R16(block + 8 + y * 2) >> (x * 4)) & 0xF;
        *a = av * 17;
    } else if (ge.tex_fmt == 10) {   /* DXT5: interpolated alpha, endpoints at block end */
        uint64_t bits = (uint64_t)MEM_R32(block + 8) | ((uint64_t)MEM_R16(block + 12) << 32);
        int code = (int)(bits >> (3 * (y * 4 + x))) & 7;
        int a0 = MEM_R8(block + 14), a1 = MEM_R8(block + 15);
        if      (code == 0) *a = a0;
        else if (code == 1) *a = a1;
        else if (a0 > a1)   *a = ((8 - code) * a0 + (code - 1) * a1) / 7;
        else if (code == 6) *a = 0;
        else if (code == 7) *a = 255;
        else                *a = ((6 - code) * a0 + (code - 1) * a1) / 5;
    }
}

static void sample_tex(int tu, int tv, int *r, int *g, int *b, int *a) {
    g_tex_samples++;
    if (ge.tex_w > 0) {
        tu = tex_coord(tu, ge.tex_w, ge.tex_wrap & 1);
        tv = tex_coord(tv, ge.tex_h, (ge.tex_wrap >> 8) & 1);
    }
    uint32_t stride = ge.tex_bufw ? ge.tex_bufw : (uint32_t)ge.tex_w;
    uint32_t u = (uint32_t)tu, v = (uint32_t)tv;
    switch (ge.tex_fmt) {
        case 0: case 1: case 2:
            unpack_color(MEM_R16(ge.tex_addr + texel_offset(stride, u, v, 16)), ge.tex_fmt, r, g, b, a); break;
        case 3:
            unpack_color(MEM_R32(ge.tex_addr + texel_offset(stride, u, v, 32)), 3, r, g, b, a); break;
        case 4: {
            uint32_t off = texel_offset(stride, u, v, 4);
            uint8_t byte = MEM_R8(ge.tex_addr + off);
            int ni = (u & 1) ? (byte >> 4) : (byte & 0xF);
            clut_lookup((uint32_t)ni, r, g, b, a); break;
        }
        case 5: clut_lookup(MEM_R8(ge.tex_addr + texel_offset(stride, u, v, 8)), r, g, b, a); break;
        case 6:  /* CLUT16: 16-bit index through the same shift/mask/start transform */
            clut_lookup(MEM_R16(ge.tex_addr + texel_offset(stride, u, v, 16)), r, g, b, a); break;
        case 7:  /* CLUT32 */
            clut_lookup(MEM_R32(ge.tex_addr + texel_offset(stride, u, v, 32)), r, g, b, a); break;
        case 8: case 9: case 10:
            sample_dxt(stride, u, v, r, g, b, a); break;
        default: *r=*g=*b=*a=255; break;
    }
    if (*r | *g | *b) g_tex_nonzero++;
    if (s_stat_on > 0 && *a == 0) s_stat.texa0++;
}

static void sample_tex_f(float fu, float fv_coord, int *r, int *g, int *b, int *a) {
    static int nobil = -1; if (nobil < 0) nobil = getenv("SR_NOBILINEAR") ? 1 : 0;
    int use_linear = !nobil && ((ge.tex_filter & 1) || ((ge.tex_filter >> 8) & 1));
    if (!use_linear) { sample_tex((int)(fu + 0.5f), (int)(fv_coord + 0.5f), r, g, b, a); return; }
    fu -= 0.5f; fv_coord -= 0.5f;
    int u0 = (int)floorf(fu), v0 = (int)floorf(fv_coord);
    int fx = (int)((fu - u0) * 256.0f), fy = (int)((fv_coord - v0) * 256.0f);
    if (fx < 0) fx = 0;
    if (fx > 256) fx = 256;
    if (fy < 0) fy = 0;
    if (fy > 256) fy = 256;
    int r0,g0,b0,a0, r1,g1,b1,a1, r2,g2,b2,a2, r3,g3,b3,a3;
    sample_tex(u0,   v0,   &r0,&g0,&b0,&a0);
    sample_tex(u0+1, v0,   &r1,&g1,&b1,&a1);
    sample_tex(u0,   v0+1, &r2,&g2,&b2,&a2);
    sample_tex(u0+1, v0+1, &r3,&g3,&b3,&a3);
#define LERP1(c0,c1,f) ((c0) + (((c1)-(c0))*(f)>>8))
    int tr=LERP1(r0,r1,fx), tg=LERP1(g0,g1,fx), tb=LERP1(b0,b1,fx), ta=LERP1(a0,a1,fx);
    int br=LERP1(r2,r3,fx), bg=LERP1(g2,g3,fx), bb=LERP1(b2,b3,fx), ba=LERP1(a2,a3,fx);
    *r=LERP1(tr,br,fy); *g=LERP1(tg,bg,fy); *b=LERP1(tb,bb,fy); *a=LERP1(ta,ba,fy);
#undef LERP1
}

/* SR_TEXDUMP=1: write each distinct texture used by a draw once per run as
 * tex_ADDR_fF_WxH.ppm (and alpha as a grayscale _a.pgm), decoded through the EXACT
 * sampler path the rasterizer uses (swizzle + CLUT included). Separates "texture decodes
 * wrong" from "UV/lighting wrong" without guessing. */
static void texdump_maybe(void) {
    static int on = -1;
    static uint32_t after = 0;
    if (on < 0) {
        on = getenv("SR_TEXDUMP") ? 1 : 0;
        const char *after_text = getenv("SR_TEXDUMP_AFTER");
        if (after_text && after_text[0]) {
            unsigned long value = strtoul(after_text, NULL, 10);
            if (value <= UINT32_MAX) after = (uint32_t)value;
        }
    }
    if (!on || s_ge_frame < after || !ge.tex_enable || !ge.tex_addr) return;
    static uint32_t seen[32]; static int n = 0;
    for (int i = 0; i < n; i++) if (seen[i] == ge.tex_addr) return;
    if (n >= 32) return;
    seen[n++] = ge.tex_addr;
    int w = (int)ge.tex_w, h = (int)ge.tex_h;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return;
    char path[160];
    snprintf(path, sizeof(path), "tex_%08x_f%u_%dx%d.ppm", ge.tex_addr, ge.tex_fmt, w, h);
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    char patha[160];
    snprintf(patha, sizeof(patha), "tex_%08x_f%u_%dx%d_a.pgm", ge.tex_addr, ge.tex_fmt, w, h);
    FILE *fa = fopen(patha, "wb");
    fprintf(fp, "P6\n%d %d\n255\n", w, h);
    if (fa) fprintf(fa, "P5\n%d %d\n255\n", w, h);
    for (int v = 0; v < h; v++)
        for (int u = 0; u < w; u++) {
            int r, g, b, a;
            sample_tex(u, v, &r, &g, &b, &a);
            unsigned char px[3] = { (unsigned char)r, (unsigned char)g, (unsigned char)b };
            fwrite(px, 1, 3, fp);
            if (fa) { unsigned char pa = (unsigned char)a; fwrite(&pa, 1, 1, fa); }
        }
    fclose(fp);
    if (fa) fclose(fa);
    fprintf(stderr, "TEXDUMP f=%u wrote %s clut=0x%08x/0x%08x sw=%u bufw=%u filter=0x%03x\n",
            s_ge_frame, path, ge.clut_addr, ge.clut_fmt, ge.tex_swizzle, ge.tex_bufw, ge.tex_filter);
}

/* GPU backend texture decode: current texture, through the exact sampler path above.
 * Linear (non-swizzled) direct-color formats take a row-wise fast path with the same
 * unpack_color expansion — render-to-texture sources are re-decoded every frame and the
 * per-texel sample_tex call (wrap + swizzle offset math) dominates otherwise. */
void ge_decode_tex_rgba(uint32_t *out) {
    if (!ge.tex_swizzle && ge.tex_fmt <= 3) {
        uint32_t stride = ge.tex_bufw ? ge.tex_bufw : (uint32_t)ge.tex_w;
        int oob = !sr_inrange(ge.tex_addr);
        if (oob) { memset(out, 0, (size_t)ge.tex_w * (size_t)ge.tex_h * 4); return; }
        for (int v = 0; v < ge.tex_h; v++) {
            uint32_t *dst = out + (uint32_t)v * (uint32_t)ge.tex_w;
            if (ge.tex_fmt == 3) {
                const uint32_t *src = (const uint32_t *)SR_HOST(ge.tex_addr + (uint32_t)v * stride * 4);
                memcpy(dst, src, (size_t)ge.tex_w * 4);
            } else {
                const uint16_t *src = (const uint16_t *)SR_HOST(ge.tex_addr + (uint32_t)v * stride * 2);
                for (int u = 0; u < ge.tex_w; u++) {
                    int r, g, b, a;
                    unpack_color(src[u], (int)ge.tex_fmt, &r, &g, &b, &a);
                    dst[u] = (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
                }
            }
        }
        return;
    }
    for (int v = 0; v < ge.tex_h; v++)
        for (int u = 0; u < ge.tex_w; u++) {
            int r, g, b, a;
            sample_tex(u, v, &r, &g, &b, &a);
            out[v * ge.tex_w + u] = (uint32_t)(r & 0xFF) | ((uint32_t)(g & 0xFF) << 8) |
                                    ((uint32_t)(b & 0xFF) << 16) | ((uint32_t)(a & 0xFF) << 24);
        }
}

static uint16_t pack565(int r, int g, int b)           { return (uint16_t)(((r>>3)&0x1F)|(((g>>2)&0x3F)<<5)|(((b>>3)&0x1F)<<11)); }
static uint16_t pack5551(int r, int g, int b, int a)   { return (uint16_t)(((r>>3)&0x1F)|(((g>>3)&0x1F)<<5)|(((b>>3)&0x1F)<<10)|(a>=128?0x8000:0)); }
static uint16_t pack4444(int r, int g, int b, int a)   { return (uint16_t)(((r>>4)&0xF)|(((g>>4)&0xF)<<4)|(((b>>4)&0xF)<<8)|(((a>>4)&0xF)<<12)); }
static uint32_t pack8888(int r, int g, int b, int a)   { return (uint32_t)(r&0xFF)|((uint32_t)(g&0xFF)<<8)|((uint32_t)(b&0xFF)<<16)|((uint32_t)(a&0xFF)<<24); }
static uint32_t pack_fb(int r, int g, int b, int a) {
    switch (ge.fbfmt & 3) {
        case 1: return pack5551(r,g,b,a);
        case 2: return pack4444(r,g,b,a);
        case 3: return pack8888(r,g,b,a);
        default: return pack565(r,g,b);
    }
}

static uint32_t ge_rel_addr(uint32_t data) {
    uint32_t base_ext = ((ge.base & 0x000F0000u) << 8) | (data & 0x00FFFFFFu);
    return (ge.offset + base_ext) & 0x0FFFFFFFu;
}

/* PSP VRAM is 2 MiB at 0x04000000, mirrored across the whole 0x04xxxxxx window: the
 * 20-bit fold below reproduces the hardware mirror, it is not a truncation.  Color
 * buffers may also legally be placed in main RAM (render-to-RAM); those addresses pass
 * through untouched instead of being folded into the VRAM window (MEM_* validates the
 * range at access time). */
static uint32_t ge_fb_addr(void) {
    /* HST emits VRAM display targets as 2 MiB-local GE offsets (0 / 0x44000).
     * Preserve explicit main-RAM targets, but fold local offsets into the PSP
     * eDRAM window. Without this, software GE overwrites low recompiled code and
     * the Vulkan backend rejects every target as non-VRAM. */
    if (ge.fbp < 0x00200000u || (ge.fbp & 0x0F000000u) == 0x04000000u)
        return 0x04000000u | (ge.fbp & 0x001FFFFFu);
    return ge.fbp & 0x0FFFFFFFu;
}

static uint32_t ge_normalize_vram_target(uint32_t addr) {
    return addr < 0x00200000u ? (0x04000000u | addr) : addr;
}

unsigned long g_ge_pixels = 0;
static int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }
static float clampf01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static void read_fb_px(int x, int y, int *r, int *g, int *b, int *a) {
    uint32_t stride = ge.fbw ? ge.fbw : 512;
    uint32_t off = (uint32_t)(y*(int)stride + x);
    if ((ge.fbfmt & 3) == 3) unpack_color(MEM_R32(ge_fb_addr() + off*4), 3, r,g,b,a);
    else                     unpack_color(MEM_R16(ge_fb_addr() + off*2), ge.fbfmt&3, r,g,b,a);
}

static int factor_component(int f, int chan, int sr, int sg, int sb, int sa, int dr, int dg, int db, int da, int srcSide) {
    int sv = chan==0?sr:(chan==1?sg:sb), dv = chan==0?dr:(chan==1?dg:db);
    int fixedA = chan==0?(ge.blend_fixa&0xFF):(chan==1?((ge.blend_fixa>>8)&0xFF):((ge.blend_fixa>>16)&0xFF));
    int fixedB = chan==0?(ge.blend_fixb&0xFF):(chan==1?((ge.blend_fixb>>8)&0xFF):((ge.blend_fixb>>16)&0xFF));
    switch (f & 0xF) {
        case 0: return srcSide?dv:sv;    case 1: return 255-(srcSide?dv:sv);
        case 2: return sa;               case 3: return 255-sa;
        case 4: return da;               case 5: return 255-da;
        case 6: return clamp8(sa*2);     case 7: return clamp8(255-sa*2);
        case 8: return clamp8(da*2);     case 9: return clamp8(255-da*2);
        case 10: default: return srcSide?fixedA:fixedB;
    }
}

static int blend_chan(int s, int d, int sf, int df, int eq) {
    int sv=s*sf/255, dv=d*df/255;
    switch (eq&7) {
        case 1: return clamp8(sv-dv); case 2: return clamp8(dv-sv);
        /* min/max/absdiff ignore the blend factors on the PSP (PPSSPP
         * Rasterizer.cpp AlphaBlendingResult operates on the raw source/dst). */
        case 3: return s<d?s:d;       case 4: return s>d?s:d;
        case 5: return abs(s-d);
        case 0: default: return clamp8(sv+dv);
    }
}

static int pixel_in_bounds(int x, int y) {
    return x >= 0 && x < 480 && y >= 0 && y < 272 &&
           x >= ge.scis_x1 && x <= ge.scis_x2 && y >= ge.scis_y1 && y <= ge.scis_y2;
}

static uint32_t read_fb_raw(int x, int y) {
    uint32_t stride = ge.fbw ? ge.fbw : 512;
    uint32_t off = (uint32_t)(y * (int)stride + x);
    return (ge.fbfmt & 3) == 3 ? MEM_R32(ge_fb_addr() + off * 4)
                               : MEM_R16(ge_fb_addr() + off * 2);
}

static void write_fb_raw(int x, int y, uint32_t value) {
    uint32_t stride = ge.fbw ? ge.fbw : 512;
    uint32_t off = (uint32_t)(y * (int)stride + x);
    if ((ge.fbfmt & 3) == 3) MEM_W32(ge_fb_addr() + off * 4, value);
    else                     MEM_W16(ge_fb_addr() + off * 2, (uint16_t)value);
}

/* PMSKC/PMSKA are RGBA8 masks but disable writes to native framebuffer bits. */
static uint32_t fb_write_mask_native(void) {
    int mr=(int)(ge.mask_rgb&0xFF), mg=(int)((ge.mask_rgb>>8)&0xFF);
    int mb=(int)((ge.mask_rgb>>16)&0xFF), ma=ge.mask_alpha&0xFF;
    switch (ge.fbfmt & 3) {
        case 0: return pack565(mr,mg,mb);
        case 1: return pack5551(mr,mg,mb,ma);
        case 2: return pack4444(mr,mg,mb,ma);
        default:return pack8888(mr,mg,mb,ma);
    }
}

static uint32_t apply_logic_op(uint32_t op, uint32_t oldc, uint32_t newc) {
    uint32_t alpha=newc&0xFF000000u, oldrgb=oldc&0x00FFFFFFu, rgb=newc&0x00FFFFFFu;
    switch (op & 15) {
        case 0: rgb=0; break;                case 1: rgb&=oldrgb; break;
        case 2: rgb&=~oldrgb; break;         case 3: break;
        case 4: rgb=(~rgb)&oldrgb; break;    case 5: rgb=oldrgb; break;
        case 6: rgb^=oldrgb; break;          case 7: rgb|=oldrgb; break;
        case 8: rgb=~(rgb|oldrgb); break;    case 9: rgb=~(rgb^oldrgb); break;
        case 10:rgb=~oldrgb; break;          case 11:rgb|=~oldrgb; break;
        case 12:rgb=~rgb; break;             case 13:rgb=(~rgb)|oldrgb; break;
        case 14:rgb=~(rgb&oldrgb); break;    case 15:rgb=0x00FFFFFFu; break;
    }
    return alpha | (rgb & 0x00FFFFFFu);
}

static int stencil_test_pass(int stencil) {
    int mask=(int)((ge.stencil_test>>16)&0xFF);
    int ref=((int)(ge.stencil_test>>8)&0xFF)&mask, value=stencil&mask;
    /* PSP stencil comparison is reference OP destination. */
    switch (ge.stencil_test & 7) {
        case 0:return 0; case 1:return 1; case 2:return ref==value; case 3:return ref!=value;
        case 4:return ref<value; case 5:return ref<=value; case 6:return ref>value; case 7:return ref>=value;
        default:return 1;
    }
}

static int apply_stencil_op(int op, int old) {
    int fmt=(int)(ge.fbfmt&3), ref=(int)((ge.stencil_test>>8)&0xFF);
    switch (op & 7) {
        case 0:return old; case 1:return 0; case 2:return ref; case 3:return (~old)&0xFF;
        case 4:
            if (fmt==3) return old<255?old+1:old;
            if (fmt==1) return 255;
            if (fmt==2) return old<0xF0?old+0x10:old;
            return old;
        case 5:
            if (fmt==2) return old>=0x10?old-0x10:old;
            if (fmt==1) return 0;
            return old>0?old-1:old;
        default:return old;
    }
}

static void write_stencil_px(int x, int y, int stencil) {
    if ((ge.fbfmt&3)==0) return;
    uint32_t oldraw=read_fb_raw(x,y);
    int r,g,b,a; unpack_color(oldraw,ge.fbfmt&3,&r,&g,&b,&a);
    uint32_t newraw=pack_fb(r,g,b,stencil), mask=fb_write_mask_native();
    write_fb_raw(x,y,(newraw&~mask)|(oldraw&mask));
}

static void compose_px_rgba(int x, int y, int r, int g, int b, int src_a, int stencil) {
    if (x<0||x>=480||y<0||y>=272) return;
    if (x<ge.scis_x1||x>ge.scis_x2||y<ge.scis_y1||y>ge.scis_y2) return;
    if (s_stat_on > 0 && s_in3d && !ge.clear && s_pxpass < 6) {
        s_pxpass++;
        fprintf(stderr, "PXPASS f=%u xy=(%d,%d) rgba=(%d,%d,%d,%d) fbp=0x%08x blend=%d/%06x\n",
                s_ge_frame, x, y, r, g, b, src_a, ge.fbp, ge.blend_enable, ge.blend_mode);
    }
    /* Centre-pixel composition trace: every write to (240,136), in order, shows what each
     * frame layer contributes and what finally covers the scene. */
    if (s_stat_on > 0 && x == 240 && y == 136 && s_pxc < 32) {
        s_pxc++;
        fprintf(stderr, "PXC f=%u rgba=(%d,%d,%d,%d) clear=%d in3d=%d tex=%d blend=%d/%06x fixab=%06x/%06x vtype=0x%06x fbp=0x%08x\n",
                s_ge_frame, r, g, b, src_a, ge.clear, s_in3d, ge.tex_enable,
                ge.blend_enable, ge.blend_mode, ge.blend_fixa, ge.blend_fixb, ge.vtype, ge.fbp);
    }
    uint32_t oldraw=read_fb_raw(x,y);
    int dr,dg,db,da; unpack_color(oldraw,ge.fbfmt&3,&dr,&dg,&db,&da);
    if (ge.clear) {
        int write_color = (ge.clear_mode & 0x100) != 0;
        int write_alpha = (ge.clear_mode & 0x200) != 0;
        if (!write_color && !write_alpha) return;
        if (!write_color) { r=dr; g=dg; b=db; }
        if (!write_alpha) stencil=da;
    } else {
        if (ge.blend_enable) {
            int eq=(int)((ge.blend_mode>>8)&7);
            int rfa=factor_component((int)(ge.blend_mode&0xF),   0,r,g,b,src_a,dr,dg,db,da,1);
            int gfa=factor_component((int)(ge.blend_mode&0xF),   1,r,g,b,src_a,dr,dg,db,da,1);
            int bfa=factor_component((int)(ge.blend_mode&0xF),   2,r,g,b,src_a,dr,dg,db,da,1);
            int rfb=factor_component((int)((ge.blend_mode>>4)&0xF),0,r,g,b,src_a,dr,dg,db,da,0);
            int gfb=factor_component((int)((ge.blend_mode>>4)&0xF),1,r,g,b,src_a,dr,dg,db,da,0);
            int bfb=factor_component((int)((ge.blend_mode>>4)&0xF),2,r,g,b,src_a,dr,dg,db,da,0);
            r=blend_chan(r,dr,rfa,rfb,eq); g=blend_chan(g,dg,gfa,gfb,eq); b=blend_chan(b,db,bfa,bfb,eq);
        }
    }
    /* Dither precedes clamping and logic, and applies in clear mode and RGBA8888 too. */
    if (ge.dither_enable) {
        int d=ge.dith[y&3][x&3]; r=clamp8(r+d); g=clamp8(g+d); b=clamp8(b+d);
    }
    uint32_t rgba=pack8888(clamp8(r),clamp8(g),clamp8(b),clamp8(stencil));
    if (ge.logic_op_enable && !ge.clear)
        rgba=apply_logic_op(ge.logic_op,pack8888(dr,dg,db,da),rgba);
    r=(int)(rgba&0xFF); g=(int)((rgba>>8)&0xFF); b=(int)((rgba>>16)&0xFF);
    stencil=(int)((rgba>>24)&0xFF);
    uint32_t c=pack_fb(r,g,b,stencil), mask=fb_write_mask_native();
    write_fb_raw(x,y,(c&~mask)|(oldraw&mask));
    g_ge_pixels++;
    if (s_in3d) s_stat.px3d++; else s_stat.px2d++;
    if (s_stat_on > 0) {
        if (s_in3d && r == 0 && g == 0 && b == 0) s_stat.blk3d++;
        uint32_t fba = ge_fb_addr();
        for (int i = 0; i < 4; i++) {
            if (s_stat.fbn[i] == 0 || s_stat.fbs[i] == fba) { s_stat.fbs[i] = fba; s_stat.fbn[i]++; break; }
        }
    }
    extern unsigned long g_list_writes, g_list_nonblack, g_list_clearpx;
    g_list_writes++;
    if (r | g | b) g_list_nonblack++;
    if (ge.clear) g_list_clearpx++;
}

static void put_px_rgba(int x, int y, int r, int g, int b, int a) {
    compose_px_rgba(x,y,r,g,b,a,a);
}

/* Ordered PSP stencil/depth commit. Alpha/color tests and fog have already run in shade(). */
static void put_px_rgba_tested(int x, int y, uint16_t pz, int use_z,
                               int r, int g, int b, int src_a) {
    if (!pixel_in_bounds(x,y)) return;
    int dr,dg,db,stencil; read_fb_px(x,y,&dr,&dg,&db,&stencil);
    if (ge.stencil_enable && !stencil_test_pass(stencil)) {
        write_stencil_px(x,y,apply_stencil_op((int)(ge.stencil_op&7),stencil));
        return;
    }
    if (use_z && !ztest_compare_px(x,y,pz)) {
        if (ge.stencil_enable)
            write_stencil_px(x,y,apply_stencil_op((int)((ge.stencil_op>>8)&7),stencil));
        return;
    }
    if (use_z && !ge.zwrite_disable) {
        uint32_t zi=(uint32_t)(y*(int)zbuf_stride()+x);
        if (zi<272u*512u) s_zbuf[zi]=pz;
    }
    if (ge.stencil_enable)
        stencil=apply_stencil_op((int)((ge.stencil_op>>16)&7),stencil);
    compose_px_rgba(x,y,r,g,b,src_a,stencil);
}

/* A rasterized vertex (GeVtx, ge_shared.h): screen position, reciprocal clip-w (for
 * persp-correct interp), UV, colour, per-vertex fog factor (1 = no fog). In transform
 * mode u/v are stored pre-multiplied by rw (texel coords over w). */
typedef GeVtx Vtx;

static void load_vtx(uint32_t addr, const VFmt *vf, Vtx *o) {
    float pz; read_pos(addr, vf, &o->x, &o->y, &pz);
    o->z = pz; o->rw = 1.0f; o->fog = 1.0f;
    read_tc(addr, vf, &o->u, &o->v);
    read_color(addr, vf, &o->r, &o->g, &o->b, &o->a);
    if (s_primitive_profile_counting) {
        s_cpu_profile_stats.primitive_profile_actual_decoded_vertices++;
        s_cpu_profile_stats.primitive_profile_actual_through_vertices++;
        primitive_profile_finish_fetch();
    }
}

static uint32_t index_value(uint32_t ibase, int idxfmt, int i) {
    switch (idxfmt) {
        case 1: return MEM_R8(ibase+(uint32_t)i);
        case 2: return MEM_R16(ibase+(uint32_t)i*2);
        case 3: return MEM_R32(ibase+(uint32_t)i*4);
        default: return (uint32_t)i;
    }
}

static uint32_t prim_vtx_addr(uint32_t vbase, uint32_t ibase, int idxfmt, int i, int stride) {
    if (s_primitive_profile_counting) {
        int phase = primitive_profile_claim_vertex();
        s_primitive_profile_pending_phase = phase;
        s_primitive_profile_pending_started =
            phase == GE_PRIM_PROFILE_VERTEX_FETCH_DECODE ? SDL_GetTicksNS() : 0;
    }
    uint32_t vi = idxfmt ? index_value(ibase, idxfmt, i) : (uint32_t)i;
    return vbase + vi * (uint32_t)stride;
}

/* Programmable alpha test (ALPHATEST 0xDB: func | ref<<8 | mask<<16). */
static int alpha_test(int a) {
    if (!ge.atest_enable) return 1;
    static int noatest = -1;
    if (noatest < 0) noatest = getenv("SR_NOATEST") ? 1 : 0;
    if (noatest) return 1;
    int mask = (int)((ge.atest >> 16) & 0xFF);
    int ref  = (int)((ge.atest >> 8) & 0xFF) & mask;
    int av   = a & mask;
    int pass;
    switch (ge.atest & 7) {
        case 0: pass = 0; break;
        case 1: pass = 1; break;
        case 2: pass = (av == ref); break;
        case 3: pass = (av != ref); break;
        case 4: pass = (av <  ref); break;
        case 5: pass = (av <= ref); break;
        case 6: pass = (av >  ref); break;
        case 7: pass = (av >= ref); break;
        default: pass = 1;
    }
    if (!pass) s_stat.afail++;
    return pass;
}

static int color_test(int r, int g, int b) {
    if (!ge.color_test_enable) return 1;
    uint32_t mask=ge.color_test_mask&0x00FFFFFFu;
    uint32_t value=pack8888(r,g,b,0)&mask;
    uint32_t ref=ge.color_ref&mask;
    switch (ge.color_test&3) {
        case 0:return 0;
        case 1:return 1;
        case 2:return value==ref;
        case 3:return value!=ref;
        default:return 1;
    }
}

/* Compute final pixel colour from interpolated texcoord+colour+fog.
 * `fog` is fixed-point 0..256 (256 = no fog). Returns 0 if the alpha test rejects. */
static int shade(float u, float fv, int fog, int vr, int vg, int vb, int va, int *or_, int *og, int *ob, int *oa) {
    int r, g, b, a;
    int dbg_ta = -1;
    if (ge.tex_enable && !ge.clear && ge.tex_addr) {
        int tr, tg, tb, ta;
        int rgba = (ge.tex_func & 0x100) != 0;
        int dbl  = (ge.tex_func & 0x10000) != 0;
        sample_tex_f(u, fv, &tr, &tg, &tb, &ta);
        dbg_ta = ta;
        switch (ge.tex_func & 7) {
            case 1: /* decal */
                if (rgba) {
                    int inva=255-ta, div=dbl?128:256;
                    r=((vr+1)*inva+(tr+1)*ta)/div; g=((vg+1)*inva+(tg+1)*ta)/div; b=((vb+1)*inva+(tb+1)*ta)/div;
                } else { r=dbl?tr*2:tr; g=dbl?tg*2:tg; b=dbl?tb*2:tb; }
                a=va; break;
            case 2: { /* blend */
                int er=ge.tex_env&0xFF, eg=(ge.tex_env>>8)&0xFF, eb=(ge.tex_env>>16)&0xFF, div=dbl?128:256;
                r=((255-tr)*vr+tr*er+255)/div; g=((255-tg)*vg+tg*eg+255)/div; b=((255-tb)*vb+tb*eb+255)/div;
                a=rgba?((va+1)*ta/256):va; break;
            }
            case 3: /* replace */
                r=dbl?tr*2:tr; g=dbl?tg*2:tg; b=dbl?tb*2:tb; a=rgba?ta:va; break;
            case 4: case 5: case 6: case 7: /* add */
                r=vr+tr; g=vg+tg; b=vb+tb;
                if (dbl){r*=2;g*=2;b*=2;}
                a=rgba?((va+1)*ta/256):va; break;
            case 0: default: /* modulate */
                r=(vr+1)*(dbl?tr*2:tr)/256; g=(vg+1)*(dbl?tg*2:tg)/256; b=(vb+1)*(dbl?tb*2:tb)/256;
                a=rgba?((va+1)*ta/256):va; break;
        }
        r=clamp8(r); g=clamp8(g); b=clamp8(b); a=clamp8(a);
    } else { r=vr; g=vg; b=vb; a=va; }
    if (!ge.clear) {
        if (!alpha_test(a)) {
            if (s_stat_on > 0 && s_in3d)
                atex_note((ge.tex_enable && ge.tex_addr) ? ge.tex_addr : 0, 0);
            if (s_stat_on > 0 && s_in3d && s_ashade < 8) {
                s_ashade++;
                /* For CLUT formats also show the sampled index byte and its raw palette entry,
                 * plus texture geometry -- distinguishes "texture really is transparent here"
                 * from "wrong texel/palette decode". */
                uint32_t rawidx = 0, rawpal = 0;
                if (ge.tex_fmt == 4 || ge.tex_fmt == 5) {
                    uint32_t stride2 = ge.tex_bufw ? ge.tex_bufw : (uint32_t)ge.tex_w;
                    uint32_t tu = (uint32_t)tex_coord((int)u, ge.tex_w, ge.tex_wrap & 1);
                    uint32_t tv = (uint32_t)tex_coord((int)fv, ge.tex_h, (ge.tex_wrap >> 8) & 1);
                    rawidx = MEM_R8(ge.tex_addr + texel_offset(stride2, tu, tv, ge.tex_fmt == 4 ? 4 : 8));
                    if (ge.tex_fmt == 4) rawidx = (tu & 1) ? (rawidx >> 4) : (rawidx & 0xF);
                    uint32_t shift2 = (ge.clut_fmt >> 2) & 0x1F, mask2 = (ge.clut_fmt >> 8) & 0xFF;
                    uint32_t cidx = ((rawidx >> shift2) & mask2);
                    if ((ge.clut_fmt & 3) == 3) memcpy(&rawpal, &ge.clutram[(cidx * 4) & 2047], 4);
                    else { uint16_t p16; memcpy(&p16, &ge.clutram[(cidx * 2) & 2047], 2); rawpal = p16; }
                }
                fprintf(stderr, "ASHADE f=%u va=%d ta=%d out_a=%d texfunc=0x%06x texfmt=%u clutfmt=0x%06x texaddr=0x%08x atest=0x%06x uv=(%.1f,%.1f) tmm=0x%03x vtype=0x%06x tsc=(%g,%g) toff=(%g,%g) tw=%d th=%d bufw=%u swz=%d idx=0x%02x pal=0x%08x lit=%d mupd=%d ama=%d ala=%d\n",
                        s_ge_frame, va, dbg_ta, a, ge.tex_func, ge.tex_fmt, ge.clut_fmt,
                        ge.tex_addr, ge.atest, u, fv, ge.tex_map_mode, ge.vtype,
                        ge.tex_scale_u, ge.tex_scale_v, ge.tex_off_u, ge.tex_off_v,
                        ge.tex_w, ge.tex_h, ge.tex_bufw, ge.tex_swizzle, rawidx, rawpal,
                        ge.lighting_enable, ge.mat_update,
                        ge.material_alpha_set ? ge.material_alpha : -1, ge.amb_alpha);
                /* Once per window: the first 16 CLUT entries and 16 raw texels, to tell a
                 * garbled/missing palette or texture upload from a wrong-texel decode. */
                if (s_ashade == 1 && (ge.tex_fmt == 4 || ge.tex_fmt == 5)) {
                    fprintf(stderr, "ACLUT f=%u addr=0x%08x pal0..15=", s_ge_frame, ge.clut_addr);
                    for (int ci = 0; ci < 16; ci++) {
                        uint32_t pv;
                        if ((ge.clut_fmt & 3) == 3) memcpy(&pv, &ge.clutram[(ci * 4) & 2047], 4);
                        else { uint16_t p16; memcpy(&p16, &ge.clutram[(ci * 2) & 2047], 2); pv = p16; }
                        fprintf(stderr, "%08x%s", pv, ci == 15 ? "" : ",");
                    }
                    fprintf(stderr, " tex0..15=");
                    for (int ti = 0; ti < 16; ti++)
                        fprintf(stderr, "%02x%s", MEM_R8(ge.tex_addr + (uint32_t)ti), ti == 15 ? "\n" : "");
                }
            }
            return 0;
        }
        if (s_stat_on > 0 && s_in3d) {
            atex_note((ge.tex_enable && ge.tex_addr) ? ge.tex_addr : 0, 1);
            if (fog == 0) s_stat.fog0++; else if (fog >= 256) s_stat.fogfull++;
        }
        if (fog < 256) {  /* mix toward fog colour; alpha unaffected */
            int fr=(int)(ge.fog_color&0xFF), fg=(int)((ge.fog_color>>8)&0xFF), fb=(int)((ge.fog_color>>16)&0xFF);
            r = fr + (((r - fr) * fog) >> 8);
            g = fg + (((g - fg) * fog) >> 8);
            b = fb + (((b - fb) * fog) >> 8);
        }
        /* Fog precedes the color test on the PSP fragment pipeline. */
        if (!color_test(r,g,b)) return 0;
    }
    *or_=r; *og=g; *ob=b; *oa=a;
    return 1;
}

/* ---- Unified triangle rasterizer ----
 * Incremental edge functions: each barycentric weight is affine in screen x/y, so it is
 * evaluated once per row and stepped per pixel; all interpolants step the same way.
 * The bounding box is pre-clipped against the screen and scissor rect.
 * persp=1 (transform-mode): back-face culling, perspective-correct UV, depth test, fog.
 * persp=0 (through-mode): affine UV; depth test only if the list enabled it. */
/* SR_PIXWHO=x,y probe pixel (shared by the triangle, sprite and clear paths). */
static int s_who_x = -2, s_who_y = -2;
static void who_init(void) {
    if (s_who_x != -2) return;
    const char *wp = getenv("SR_PIXWHO");
    s_who_x = s_who_y = -1;
    if (wp) sscanf(wp, "%d,%d", &s_who_x, &s_who_y);
}

static void raster_tri(const Vtx *A, const Vtx *B, const Vtx *C, int persp) {
    /* GPU capture seam: a registered backend takes the triangle (and reproduces culling,
     * depth, shading, blending on the GPU); if it declines it has already flushed, so the
     * software path below stays correctly ordered against prior GPU work. */
    if (s_gpu && s_gpu->tri) {
        uint64_t profile_started = ge_cpu_profile_begin();
        int handled = s_gpu->tri(A, B, C, persp);
        ge_cpu_profile_end(GE_CPU_GPU_HOOK, profile_started);
        if (handled) return;
    }

    float area=(B->x-A->x)*(C->y-A->y)-(C->x-A->x)*(B->y-A->y);
    if (area == 0.0f) { if (persp) s_stat.deg3d++; return; }

    /* Back-face culling, PPSSPP model (BinManager::AddTriangle): the rasterizer only draws
     * triangles whose SUBMITTED order has positive cross product (CCW in y-down screen
     * space); the cull MODE is realised by the caller submitting the vertices in straight
     * or reversed order (and flipping that per strip-triangle parity) — see draw_prim.
     * Applies in BOTH transform and through mode (TransformUnit computes cullType before
     * the through check). */
    static int nocull = -1;
    if (nocull < 0) nocull = getenv("SR_NOCULL") ? 1 : 0;
    if (ge.cull_enable && !ge.clear && !nocull) {
        if (area < 0.0f) { s_stat.cull++; return; }
    }
    if (persp) s_stat.tri3d++; else s_stat.tri2d++;

    int minx=(int)floorf(fminf(A->x,fminf(B->x,C->x))), maxx=(int)ceilf(fmaxf(A->x,fmaxf(B->x,C->x)));
    int miny=(int)floorf(fminf(A->y,fminf(B->y,C->y))), maxy=(int)ceilf(fmaxf(A->y,fmaxf(B->y,C->y)));
    int cx1 = ge.scis_x1 > 0 ? ge.scis_x1 : 0,   cy1 = ge.scis_y1 > 0 ? ge.scis_y1 : 0;
    int cx2 = ge.scis_x2 < 479 ? ge.scis_x2 : 479, cy2 = ge.scis_y2 < 271 ? ge.scis_y2 : 271;
    if (minx < cx1) minx = cx1;
    if (maxx > cx2) maxx = cx2;
    if (miny < cy1) miny = cy1;
    if (maxy > cy2) maxy = cy2;
    if (minx > maxx || miny > maxy) return;

    /* Coverage uses EXACT integer edge functions on coordinates snapped to the PSP's native
     * 28.4 subpixel grid, with a top-left fill rule. The previous float test (w<0 reject)
     * evaluated shared edges with different rounding in each neighbouring triangle, so dense
     * meshes both dropped pixels (cracks -> models dissolve into noise) and double-drew them
     * (blended seams). Integer edges give each edge pixel to exactly one triangle. */
    /* Hardware rounding: (int)(x*16 + 0.375) per PPSSPP ClipToScreenInternal. */
    long long ax=(long long)floorf(A->x*16.0f+0.375f), ay=(long long)floorf(A->y*16.0f+0.375f);
    long long bx=(long long)floorf(B->x*16.0f+0.375f), by=(long long)floorf(B->y*16.0f+0.375f);
    long long cxx=(long long)floorf(C->x*16.0f+0.375f), cyy=(long long)floorf(C->y*16.0f+0.375f);
    long long area2 = (bx-ax)*(cyy-ay) - (cxx-ax)*(by-ay);
    if (area2 == 0) { if (persp) s_stat.deg3d++; return; }
    const Vtx *VA=A, *VB=B, *VC=C;
    if (area2 < 0) {  /* normalise winding so the interior is the positive side */
        const Vtx *t=VB; VB=VC; VC=t;
        long long tt; tt=bx; bx=cxx; cxx=tt; tt=by; by=cyy; cyy=tt;
        area2 = -area2;
    }

    float inv = 1.0f / (float)area2;

    int textured = ge.tex_enable && !ge.clear && ge.tex_addr;
    int gouraud  = ge.shade_gouraud;
    int use_z    = persp ? ge.ztest_enable : thru_ztest();
    int use_fog  = persp && ge.fog_enable && !ge.clear;
    s_in3d = persp;

    /* Attribute deltas relative to VC: attr = VCattr + w0*(VA-VC) + w1*(VB-VC). Attributes are
     * recomputed per covered pixel from the integer weights, so identical geometry always
     * produces identical z/uv (no incremental float drift between passes). */
#define AT_SETUP(n, f) float n##_ac=(VA->f)-(VC->f), n##_bc=(VB->f)-(VC->f)
#define AT_PIX(n, f)   float n=(VC->f)+w0f*n##_ac+w1f*n##_bc
    AT_SETUP(iu, u); AT_SETUP(iv, v); AT_SETUP(irw, rw); AT_SETUP(iz, z); AT_SETUP(ifog, fog);
    AT_SETUP(ir, r); AT_SETUP(ig, g); AT_SETUP(ib, b); AT_SETUP(ia, a);

    /* Edge functions at pixel centres: e0 along VA->VB (weights VC), e1 along VB->VC
     * (weights VA), e2 along VC->VA (weights VB). Bias 0 keeps E==0 pixels only on
     * top (dy==0, dx>0) and left (dy<0) edges. */
    long long dx0=bx-ax,  dy0=by-ay;
    long long dx1=cxx-bx, dy1=cyy-by;
    long long dx2=ax-cxx, dy2=ay-cyy;
    long long bias0 = (dy0 < 0 || (dy0 == 0 && dx0 > 0)) ? 0 : -1;
    long long bias1 = (dy1 < 0 || (dy1 == 0 && dx1 > 0)) ? 0 : -1;
    long long bias2 = (dy2 < 0 || (dy2 == 0 && dx2 > 0)) ? 0 : -1;
    long long px0 = (long long)minx*16 + 8, py0 = (long long)miny*16 + 8;
    long long e0r = dx0*(py0-ay)  - dy0*(px0-ax);
    long long e1r = dx1*(py0-by)  - dy1*(px0-bx);
    long long e2r = dx2*(py0-cyy) - dy2*(px0-cxx);

    for (int y=miny; y<=maxy; y++, e0r+=dx0*16, e1r+=dx1*16, e2r+=dx2*16) {
        long long e0=e0r, e1=e1r, e2=e2r;
        for (int x=minx; x<=maxx; x++, e0-=dy0*16, e1-=dy1*16, e2-=dy2*16) {
            if ((e0+bias0) < 0 || (e1+bias1) < 0 || (e2+bias2) < 0) continue;
            float w0f = (float)e1*inv, w1f = (float)e2*inv;
            AT_PIX(irw, rw); AT_PIX(iz, z); AT_PIX(ifog, fog);
            if (!zrange_ok(iz, persp)) { if (s_stat_on > 0) s_stat.zrange++; continue; }

            /* Clear-mode triangles: depth (bit 10) writes unconditionally, no z-test — the
             * normal path below only touches the z-buffer when z-test is enabled, so a game
             * clearing depth with triangles would leave stale z behind (mass z-rejects). */
            if (ge.clear) {
                if (ge.clear_mode & 0x400) {
                    uint32_t zi = (uint32_t)(y * (int)zbuf_stride() + x);
                    if (zi < 272u*512u) s_zbuf[zi] = clamp_z(iz);
                }
                who_init();
                if (x == s_who_x && y == s_who_y && s_pixwho_n < 400) {
                    s_pixwho_n++;
                    fprintf(stderr, "PIXWHO f=%u CLEARTRI mode=0x%03x z=%.0f persp=%d rgba=(%d,%d,%d,%d)\n",
                            s_ge_frame, ge.clear_mode, iz, persp, C->r, C->g, C->b, C->a);
                }
                if (ge.clear_mode & 0x300)
                    put_px_rgba(x, y, C->r, C->g, C->b, C->a);
                continue;
            }

            float u = 0, fv = 0;
            if (textured) {
                AT_PIX(iu, u); AT_PIX(iv, v);
                if (persp) {
                    float inv_rw = (irw != 0.0f) ? 1.0f / irw : 1.0f;
                    u = iu * inv_rw; fv = iv * inv_rw;
                } else { u = iu; fv = iv; }
            }
            int r, g, b, a;
            if (gouraud) {
                AT_PIX(ir, r); AT_PIX(ig, g); AT_PIX(ib, b); AT_PIX(ia, a);
                r=(int)ir; g=(int)ig; b=(int)ib; a=(int)ia;
            }
            else         { r=C->r; g=C->g; b=C->b; a=C->a; }
            int fog = use_fog ? (int)(clampf01(ifog) * 256.0f) : 256;

            int sr,sg,sb,sa;
            if (!shade(u,fv,fog,r,g,b,a,&sr,&sg,&sb,&sa)) continue;

            /* SR_PIXWHO=x,y: log every triangle that writes the chosen pixel — identifies
             * which draw owns a misrendered region without guessing from geometry. */
            {
                who_init();
                if (x == s_who_x && y == s_who_y && s_pixwho_n < 400) {
                    s_pixwho_n++;
                    uint32_t zidx = (uint32_t)(y * (int)zbuf_stride() + x);
                    unsigned curz = zidx < 272u*512u ? s_zbuf[zidx] : 99999u;
                    /* zpass: the verdict the depth test will actually give this pixel. */
                    int zpass = 1;
                    if (use_z && zidx < 272u*512u) {
                        uint16_t zv = clamp_z(iz);
                        switch (ge.ztest & 7) {
                            case 0: zpass = 0; break;
                            case 2: zpass = (zv == curz); break;
                            case 3: zpass = (zv != curz); break;
                            case 4: zpass = (zv <  curz); break;
                            case 5: zpass = (zv <= curz); break;
                            case 6: zpass = (zv >  curz); break;
                            case 7: zpass = (zv >= curz); break;
                            default: zpass = 1;
                        }
                    }
                    fprintf(stderr, "PIXWHO f=%u d=%u persp=%d clear=%d vtype=0x%06x tex=%d/0x%08x vcol=(%d,%d,%d,%d) uv=(%.2f,%.2f) rgba=(%d,%d,%d,%d) fog=%d z=%.0f zt=%d/%d zcur=%u zpass=%d zw=%d zr=[%u,%u] blend=%d/%06x world_t=(%.4g,%.4g,%.4g) tri=A(%.1f,%.1f,%.0f)B(%.1f,%.1f,%.0f)C(%.1f,%.1f,%.0f)\n",
                            s_ge_frame, s_rt_draw, persp, ge.clear, ge.vtype, ge.tex_enable, ge.tex_addr,
                            r, g, b, a, u, fv,
                            sr, sg, sb, sa, fog, iz,
                            use_z ? (int)(ge.ztest & 7) : -1, ge.ztest_enable, curz, zpass,
                            !ge.zwrite_disable, ge.minz, ge.maxz,
                            ge.blend_enable, ge.blend_mode,
                            ge.world[9], ge.world[10], ge.world[11],
                            A->x, A->y, A->z, B->x, B->y, B->z, C->x, C->y, C->z);
                }
            }
            put_px_rgba_tested(x,y,clamp_z(iz),use_z,sr,sg,sb,sa);
        }
    }
#undef AT_SETUP
#undef AT_PIX
}

/* Line rasterizer (DDA), used for prim types 1/2 in both modes. */
static void draw_line_vtx(const Vtx *A, const Vtx *B, int persp) {
    if (s_gpu && s_gpu->line) {
        uint64_t profile_started = ge_cpu_profile_begin();
        int handled = s_gpu->line(A, B, persp);
        ge_cpu_profile_end(GE_CPU_GPU_HOOK, profile_started);
        if (handled) return;
    }
    float dx=B->x-A->x, dy=B->y-A->y;
    float len=fmaxf(fabsf(dx),fabsf(dy));
    int steps=(int)len;
    if (steps<1) steps=1;
    if (steps>2048) steps=2048;
    int textured = ge.tex_enable && !ge.clear && ge.tex_addr;
    int use_z    = persp ? ge.ztest_enable : thru_ztest();
    int use_fog  = persp && ge.fog_enable && !ge.clear;
    s_in3d = persp;
    for (int i=0; i<=steps; i++) {
        float t=(float)i/(float)steps;
        int x=(int)(A->x+dx*t), y=(int)(A->y+dy*t);
        float u=0, fv=0;
        if (textured) {
            u  = A->u + (B->u - A->u)*t;
            fv = A->v + (B->v - A->v)*t;
            if (persp) {
                float rw = A->rw + (B->rw - A->rw)*t;
                if (rw != 0.0f) { u /= rw; fv /= rw; }
            }
        }
        int r=(int)(A->r+(B->r-A->r)*t), g=(int)(A->g+(B->g-A->g)*t);
        int b=(int)(A->b+(B->b-A->b)*t), a=(int)(A->a+(B->a-A->a)*t);
        if (!ge.shade_gouraud) { r=B->r; g=B->g; b=B->b; a=B->a; }
        float lz = A->z+(B->z-A->z)*t;
        if (!zrange_ok(lz, persp)) { if (s_stat_on > 0) s_stat.zrange++; continue; }
        int fog = use_fog ? (int)(clampf01(A->fog+(B->fog-A->fog)*t)*256.0f) : 256;
        int sr,sg,sb,sa;
        if (!shade(u,fv,fog,r,g,b,a,&sr,&sg,&sb,&sa)) continue;
        put_px_rgba_tested(x,y,clamp_z(lz),use_z,sr,sg,sb,sa);
    }
}

static void draw_point_vtx(const Vtx *A, int persp) {
    if (s_gpu && s_gpu->point) {
        uint64_t profile_started = ge_cpu_profile_begin();
        int handled = s_gpu->point(A, persp);
        ge_cpu_profile_end(GE_CPU_GPU_HOOK, profile_started);
        if (handled) return;
    }
    if (!zrange_ok(A->z, persp)) { if (s_stat_on > 0) s_stat.zrange++; return; }
    int textured = ge.tex_enable && !ge.clear && ge.tex_addr;
    float u=0, fv=0;
    if (textured) {
        u=A->u; fv=A->v;
        if (persp && A->rw != 0.0f) { u/=A->rw; fv/=A->rw; }
    }
    int fog = (persp && ge.fog_enable && !ge.clear) ? (int)(clampf01(A->fog)*256.0f) : 256;
    s_in3d = persp;
    int sr,sg,sb,sa;
    if (!shade(u,fv,fog,A->r,A->g,A->b,A->a,&sr,&sg,&sb,&sa)) return;
    int use_z = persp ? ge.ztest_enable : thru_ztest();
    put_px_rgba_tested((int)A->x,(int)A->y,clamp_z(A->z),use_z,sr,sg,sb,sa);
}

/* ---- 3D T&L pipeline ---- */

/* Multiply 4×3 column-major matrix by vec3. Returns vec3. (World/View transforms) */
static void mat43_mul(const float *m, float ix, float iy, float iz, float *ox, float *oy, float *oz) {
    *ox = m[0]*ix + m[3]*iy + m[6]*iz + m[9];
    *oy = m[1]*ix + m[4]*iy + m[7]*iz + m[10];
    *oz = m[2]*ix + m[5]*iy + m[8]*iz + m[11];
}

/* Rotate a vec3 by the upper 3×3 of a 4×3 matrix (no translation; for normals). */
static void mat33_mul(const float *m, float ix, float iy, float iz, float *ox, float *oy, float *oz) {
    *ox = m[0]*ix + m[3]*iy + m[6]*iz;
    *oy = m[1]*ix + m[4]*iy + m[7]*iz;
    *oz = m[2]*ix + m[5]*iy + m[8]*iz;
}

/* Multiply 4×4 column-major matrix by vec3, returning vec4 (clip space). (Proj transform) */
static void mat44_mul(const float *m, float ix, float iy, float iz, float *ox, float *oy, float *oz, float *ow) {
    *ox = m[0]*ix + m[4]*iy + m[8]*iz  + m[12];
    *oy = m[1]*ix + m[5]*iy + m[9]*iz  + m[13];
    *oz = m[2]*ix + m[6]*iy + m[10]*iz + m[14];
    *ow = m[3]*ix + m[7]*iy + m[11]*iz + m[15];
}

static float c8f(uint32_t col, int chan) { return (float)((col >> (chan*8)) & 0xFF) * (1.0f/255.0f); }

/* Per-vertex lighting in world space: emissive + scene ambient, plus per-light
 * ambient/diffuse/specular for up to 4 directional/point lights (spot treated as point).
 * Material colours come from the registers or the vertex colour per MATERIALUPDATE bits. */
static void light_vertex(float wx, float wy, float wz, float nx, float ny, float nz,
                         int vr, int vg, int vb, int va, int *lr, int *lg, int *lb, int *la) {
    float vrf = vr*(1.0f/255.0f), vgf = vg*(1.0f/255.0f), vbf = vb*(1.0f/255.0f);
    /* Lit alpha = material-ambient alpha * scene-ambient alpha; diffuse/specular/emissive never
     * contribute to alpha (PPSSPP Software/Lighting.cpp). With MATERIALUPDATE bit 0 the vertex
     * colour (including its alpha) substitutes for the ambient material colour. */
    int maa = (ge.mat_update & 1) ? va : (ge.material_alpha_set ? ge.material_alpha : 255);
    *la = clamp8((maa * ge.amb_alpha + 127) / 255);
    float mar, mag, mab, mdr, mdg, mdb, msr, msg, msb;
    if (ge.mat_update & 1) { mar=vrf; mag=vgf; mab=vbf; }
    else { mar=c8f(ge.material,0); mag=c8f(ge.material,1); mab=c8f(ge.material,2); }
    if (ge.mat_update & 2) { mdr=vrf; mdg=vgf; mdb=vbf; }
    else { mdr=c8f(ge.mat_diffuse,0); mdg=c8f(ge.mat_diffuse,1); mdb=c8f(ge.mat_diffuse,2); }
    if (ge.mat_update & 4) { msr=vrf; msg=vgf; msb=vbf; }
    else { msr=c8f(ge.mat_specular,0); msg=c8f(ge.mat_specular,1); msb=c8f(ge.mat_specular,2); }

    float r = c8f(ge.mat_emissive,0) + c8f(ge.amb_color,0)*mar;
    float g = c8f(ge.mat_emissive,1) + c8f(ge.amb_color,1)*mag;
    float b = c8f(ge.mat_emissive,2) + c8f(ge.amb_color,2)*mab;

    for (int i = 0; i < 4; i++) {
        if (!ge.light_on[i]) continue;
        int comp = (int)(ge.light_type[i] & 3);
        int type = (int)((ge.light_type[i] >> 8) & 3);
        float lx = ge.light_pos[i][0], ly = ge.light_pos[i][1], lz = ge.light_pos[i][2];
        float att = 1.0f;
        if (type != 0) {  /* point/spot: light vector from vertex, distance attenuation */
            lx -= wx; ly -= wy; lz -= wz;
            float d2 = lx*lx + ly*ly + lz*lz;
            float d = sqrtf(d2);
            float den = ge.light_att[i][0] + ge.light_att[i][1]*d + ge.light_att[i][2]*d2;
            att = den > 0.0f ? clampf01(1.0f/den) : 1.0f;
            if (d > 0.0f) { lx/=d; ly/=d; lz/=d; }
        } else {          /* directional: position register holds the direction */
            float d = sqrtf(lx*lx + ly*ly + lz*lz);
            if (d > 0.0f) { lx/=d; ly/=d; lz/=d; }
        }
        float ndl = nx*lx + ny*ly + nz*lz;
        if (ndl < 0.0f) ndl = 0.0f;
        if (comp == 2 && ge.spec_coef > 0.0f) ndl = powf(ndl, ge.spec_coef);   /* powered diffuse */

        r += att*(c8f(ge.light_acol[i],0)*mar + c8f(ge.light_dcol[i],0)*mdr*ndl);
        g += att*(c8f(ge.light_acol[i],1)*mag + c8f(ge.light_dcol[i],1)*mdg*ndl);
        b += att*(c8f(ge.light_acol[i],2)*mab + c8f(ge.light_dcol[i],2)*mdb*ndl);

        if ((comp & 1) && ndl > 0.0f) {  /* specular, Blinn half-vector with eye (0,0,1) */
            float hx=lx, hy=ly, hz=lz+1.0f;
            float hd = sqrtf(hx*hx + hy*hy + hz*hz);
            if (hd > 0.0f) {
                float ndh = (nx*hx + ny*hy + nz*hz) / hd;
                if (ndh > 0.0f) {
                    float s = ge.spec_coef > 0.0f ? powf(ndh, ge.spec_coef) : ndh;
                    r += att*c8f(ge.light_scol[i],0)*msr*s;
                    g += att*c8f(ge.light_scol[i],1)*msg*s;
                    b += att*c8f(ge.light_scol[i],2)*msb*s;
                }
            }
        }
    }
    *lr = clamp8((int)(r*255.0f + 0.5f));
    *lg = clamp8((int)(g*255.0f + 0.5f));
    *lb = clamp8((int)(b*255.0f + 0.5f));
}

typedef struct {
    float x,y,z,nx,ny,nz,u,v;
    int r,g,b,a;
} ModelVtx;

/* Decode morph targets first, then apply all active bone weights.  Patch tessellation consumes
 * these model-space values; doing either operation after tessellation changes the surface. */
static void decode_model_vtx(uint32_t addr, const VFmt *vf, ModelVtx *m) {
    read_pos(addr,vf,&m->x,&m->y,&m->z);
    read_nrm(addr,vf,&m->nx,&m->ny,&m->nz);
    read_tc(addr,vf,&m->u,&m->v);
    read_color(addr,vf,&m->r,&m->g,&m->b,&m->a);
    if (!vf->through && vf->w_n) {
        float w[8],px=0,py=0,pz=0,nx=0,ny=0,nz=0;
        read_weights(addr,vf,w);
        for (int i=0;i<vf->w_n;i++) {
            if (w[i]==0.0f) continue;
            float tx,ty,tz;
            mat43_mul(ge.bone+12*i,m->x,m->y,m->z,&tx,&ty,&tz);
            px+=tx*w[i]; py+=ty*w[i]; pz+=tz*w[i];
            mat33_mul(ge.bone+12*i,m->nx,m->ny,m->nz,&tx,&ty,&tz);
            nx+=tx*w[i]; ny+=ty*w[i]; nz+=tz*w[i];
        }
        m->x=px; m->y=py; m->z=pz; m->nx=nx; m->ny=ny; m->nz=nz;
    }
    if (s_primitive_profile_counting)
        primitive_profile_finish_fetch();
}

/* Transform a model-space vertex through the T&L pipeline and write screen-space result into *o.
 * Returns 0 if the vertex projects to non-positive clip w (behind the camera). */
/* Clip-space vertex: position before the perspective divide plus raw (un-premultiplied)
 * texel UV and lit color, so triangles crossing the near plane can be clipped with plain
 * linear interpolation and only then projected. The old pipeline rejected any vertex with
 * clipw <= 0, which discarded every triangle touching the camera plane — in close-up
 * scenes (the hangar floor/aircraft) that erased whole models. */
typedef struct {
    float cx, cy, cz, cw; float u, v; float fog; int r, g, b, a;
    /* PSP primitive-acceptance flags (PPSSPP Clipper::ProcessTriangle / ClipToScreenInternal):
     * oor = projected screen coords outside the GE's 4096-grid; ozp/ozn = z/w beyond +-1. */
    int oor, ozp, ozn;
    /* Projected screen-grid coords (pre-offset) kept for SR_RTRACE decision logging; only
     * meaningful when cw != 0. */
    float sx, sy, sz;
} CVtx;

static void transform_model_vtx_clip(const ModelVtx *m, CVtx *o) {
    int profile_phase = s_primitive_profile_current_phase;
    uint64_t transform_started = profile_phase == GE_PRIM_PROFILE_TRANSFORM
        ? SDL_GetTicksNS() : 0;
    float mx=m->x, my=m->y, mz=m->z;

    /* World transform (model → world) */
    float wx, wy, wz;
    mat43_mul(ge.world, mx, my, mz, &wx, &wy, &wz);

    /* View transform (world → camera) */
    float cx, cy, cz;
    mat43_mul(ge.view, wx, wy, wz, &cx, &cy, &cz);

    /* Projection (camera → clip). The divisor is the real clip-space w from the projection
     * matrix W row: perspective matrices (sceGumPerspective) have W row (0,0,-1,0), so
     * clipw == -cz; orthographic matrices have W row (0,0,0,1), so clipw == 1 and view
     * depth must NOT divide (hangar/menu 3D and overlay quads use ortho). */
    mat44_mul(ge.proj, cx, cy, cz, &o->cx, &o->cy, &o->cz, &o->cw);

    /* Fog factor from view-space depth: 1 = no fog, 0 = full fog (PPSSPP convention).
     * Linear in cz, so interpolating the factor across a clipped edge stays exact. */
    static int nofog = -1;
    if (nofog < 0) nofog = getenv("SR_NOFOG") ? 1 : 0;
    if (ge.fog_enable && !nofog) {
        /* Inf/NaN fog coefficients are VALID on the PSP: games leave fog enabled but set
         * fog-end to inf/nan to force a constant factor (PPSSPP TransformUnit ComputeState,
         * "required for Outrun to render proper skies"). Plain float math turns these into
         * NaN, which clampf01 passes through -> full fog -> solid fog-coloured geometry. */
        float fend = ge.fog_end, fslope = ge.fog_slope;
        if (isinf(fend) || isnan(fend)) {
            int sign = signbit(fend) ? 1 : 0;
            if (signbit(fslope)) sign = !sign;   /* the multiply would flip it */
            if (fslope == 0.0f) sign = 1;        /* *0 clamps to zero regardless of sign */
            o->fog = sign ? 0.0f : 1.0f;
        } else {
            if (isinf(fslope) || isnan(fslope))
                fslope = signbit(fslope) ? -262144.0f : 262144.0f;
            o->fog = clampf01((cz + fend) * fslope);
        }
    } else o->fog = 1.0f;

    /* Texcoords. Mode 0: vertex UVs with TEXSCALE/TEXOFFSET. Mode 1: texgen matrix applied
     * to a source picked by bits 8-9 (0=model position, 1=vertex uv, 2=normalized normal,
     * 3=raw normal); scale/offset are NOT applied in this mode (PPSSPP TransformUnit).
     * Mode 2: environment map from two light directions. Result is in texel units;
     * the 1/w premultiply for perspective-correct interpolation happens at projection. */
    float u, fv;
    uint32_t tmm = ge.tex_map_mode & 3;
    if (tmm == 1) {
        float sx, sy, sz2;
        switch ((ge.tex_map_mode >> 8) & 3) {
        case 0:  sx = mx; sy = my; sz2 = mz; break;
        case 1:  sx=m->u; sy=m->v; sz2=0.0f; break;
        default: {
            sx=m->nx; sy=m->ny; sz2=m->nz;
            if (((ge.tex_map_mode >> 8) & 3) == 2) {
                float nl = sqrtf(sx*sx + sy*sy + sz2*sz2);
                if (nl > 0.0f) { sx /= nl; sy /= nl; sz2 /= nl; }
            }
            break;
        }
        }
        float ts, tt, tq;
        mat43_mul(ge.tgen, sx, sy, sz2, &ts, &tt, &tq);
        if (tq == 0.0f) tq = 1.0f;   /* affine texgen has q==... (z row unused) */
        u  = (ts / tq) * (float)ge.tex_w;
        fv = (tt / tq) * (float)ge.tex_h;
    } else if (tmm == 2) {
        /* env map: s/t = (1 + dot(normalize(lightpos[lsN]), worldnormal)) / 2 */
        float nx2, ny2, nz2, wnx2, wny2, wnz2;
        nx2=m->nx; ny2=m->ny; nz2=m->nz;
        mat33_mul(ge.world, nx2, ny2, nz2, &wnx2, &wny2, &wnz2);
        float nl2 = sqrtf(wnx2*wnx2 + wny2*wny2 + wnz2*wnz2);
        if (nl2 > 0.0f) { wnx2 /= nl2; wny2 /= nl2; wnz2 /= nl2; }
        float st2[2];
        for (int li = 0; li < 2; li++) {
            int l = (int)((ge.tex_shade_ls >> (li * 8)) & 3);
            float lx = ge.light_pos[l][0], ly = ge.light_pos[l][1], lz = ge.light_pos[l][2];
            float ld = sqrtf(lx*lx + ly*ly + lz*lz);
            if (ld > 0.0f) { lx /= ld; ly /= ld; lz /= ld; }
            st2[li] = (1.0f + (lx*wnx2 + ly*wny2 + lz*wnz2)) * 0.5f;
        }
        u  = st2[0] * (float)ge.tex_w;
        fv = st2[1] * (float)ge.tex_h;
    } else {
        u=m->u; fv=m->v;
        u  = (u  * ge.tex_scale_u + ge.tex_off_u) * (float)ge.tex_w;
        fv = (fv * ge.tex_scale_v + ge.tex_off_v) * (float)ge.tex_h;
    }
    o->u = u;
    o->v = fv;

    o->r=m->r; o->g=m->g; o->b=m->b; o->a=m->a;

    if (transform_started) {
        primitive_profile_end(GE_PRIM_PROFILE_TRANSFORM, transform_started);
        transform_started = 0;
    }

    static int nolight = -1;
    if (nolight < 0) nolight = getenv("SR_NOLIGHT") ? 1 : 0;
    if (ge.lighting_enable && !nolight) {
        uint64_t lighting_started = profile_phase == GE_PRIM_PROFILE_LIGHTING
            ? SDL_GetTicksNS() : 0;
        float nx=m->nx, ny=m->ny, nz=m->nz;
        float wnx, wny, wnz;
        mat33_mul(ge.world, nx, ny, nz, &wnx, &wny, &wnz);
        float nl = sqrtf(wnx*wnx + wny*wny + wnz*wnz);
        if (nl > 0.0f) { wnx/=nl; wny/=nl; wnz/=nl; }
        light_vertex(wx, wy, wz, wnx, wny, wnz, o->r, o->g, o->b, o->a, &o->r, &o->g, &o->b, &o->a);
        if (lighting_started)
            primitive_profile_end(GE_PRIM_PROFILE_LIGHTING, lighting_started);
    }

    /* PSP acceptance flags (PPSSPP ClipToScreenInternal + CheckOutsideZ). The hardware does
     * NOT rasterise primitives whose vertices leave the valid screen grid or the z range —
     * games count on this to hide effect passes and behind-camera geometry. */
    o->oor = o->ozp = o->ozn = 0;
    {
        const float zoutside = 1.000030517578125f;
        float zw = o->cz / o->cw;
        if (zw >= zoutside) o->ozp = 1;
        else if (-zw >= zoutside) o->ozn = 1;
        float sx = o->cx * decode_float24(ge.viewport_raw[0]) / o->cw + decode_float24(ge.viewport_raw[3]);
        float sy = o->cy * decode_float24(ge.viewport_raw[1]) / o->cw + decode_float24(ge.viewport_raw[4]);
        float sz = o->cz * decode_float24(ge.viewport_raw[2]) / o->cw + decode_float24(ge.viewport_raw[5]);
        o->sx = sx; o->sy = sy; o->sz = sz;
        const float SB = 4095.0f + 15.5f / 16.0f;
        if (ge.depth_clip) {
            if (o->cz > -o->cw && (sx >= SB || sy >= SB || sx < 0.0f || sy < 0.0f)) o->oor = 1;
        } else {
            if (sx > SB || sy >= SB || sx < 0.0f || sy < 0.0f || sz < 0.0f || sz >= 65536.0f) o->oor = 1;
        }
    }
}

static void transform_vtx_clip(uint32_t addr, const VFmt *vf, CVtx *o) {
    int profile_phase = s_primitive_profile_counting ? s_primitive_profile_pending_phase : -1;
    ModelVtx m;
    if (s_primitive_profile_counting) {
        s_cpu_profile_stats.primitive_profile_actual_decoded_vertices++;
        s_cpu_profile_stats.primitive_profile_actual_transformed_vertices++;
    }
    decode_model_vtx(addr,vf,&m);
    /* decode_model_vtx closes the fetch/decode timer.  The other sampled
     * phases cover only the transform routine below. */
    if (s_primitive_profile_counting)
        s_primitive_profile_current_phase = profile_phase;
    transform_model_vtx_clip(&m,o);
    if (s_primitive_profile_counting) {
        s_primitive_profile_current_phase = -1;
        primitive_profile_clear_vertex_pending();
    }
}

/* Smallest clip-space w kept by the near clipper. */
#define NEAR_W 1e-3f

/* Perspective divide + viewport: clip-space vertex (cw > 0 required) → screen vertex. */
static void project_cvtx(const CVtx *c, Vtx *o) {
    float inv_w = 1.0f / c->cw;
    float ndcx = c->cx * inv_w, ndcy = c->cy * inv_w, ndcz = c->cz * inv_w;

    float vp_xs = decode_float24(ge.viewport_raw[0]);
    float vp_ys = decode_float24(ge.viewport_raw[1]);
    float vp_zs = decode_float24(ge.viewport_raw[2]);
    float vp_xc = decode_float24(ge.viewport_raw[3]);
    float vp_yc = decode_float24(ge.viewport_raw[4]);
    float vp_zc = decode_float24(ge.viewport_raw[5]);
    float off_x = (float)(ge.offsetx & 0xFFFF) / 16.0f;
    float off_y = (float)(ge.offsety & 0xFFFF) / 16.0f;

    o->x  = ndcx * vp_xs + vp_xc - off_x;
    o->y  = ndcy * vp_ys + vp_yc - off_y;
    float sz = ndcz * vp_zs + vp_zc;
    o->z  = sz < 0.0f ? 0.0f : sz > 65535.0f ? 65535.0f : sz;
    o->rw = inv_w;          /* 1/clipw for perspective-correct UV interpolation */
    o->u  = c->u * inv_w;
    o->v  = c->v * inv_w;
    o->fog = c->fog;
    o->r = c->r; o->g = c->g; o->b = c->b; o->a = c->a;
}

/* Compatibility wrapper for points/lines/sprites: reject behind-camera vertices. */
static int transform_vtx_3d(uint32_t addr, const VFmt *vf, Vtx *o) {
    CVtx c;
    transform_vtx_clip(addr, vf, &c);
    if (c.cw <= NEAR_W) {
        if (s_primitive_profile_counting) s_cpu_profile_stats.primitive_profile_vertex_rejects++;
        s_stat.nearclip++; return 0;
    }
    /* PSP point/sprite acceptance: out-of-range or out-of-z vertices are dropped whole
     * (PPSSPP Clipper::ProcessRect / ProcessPoint; rects do not clip against any plane). */
    if (c.oor) {
        if (s_primitive_profile_counting) s_cpu_profile_stats.primitive_profile_vertex_rejects++;
        s_stat.nearclip++; return 0;
    }
    if (!ge.depth_clip && (c.ozp || c.ozn)) {
        if (s_primitive_profile_counting) s_cpu_profile_stats.primitive_profile_vertex_rejects++;
        s_stat.nearclip++; return 0;
    }
    project_cvtx(&c, o);
    return 1;
}

static void clip_lerp_cvtx(const CVtx *a, const CVtx *b, float t, CVtx *o) {
    o->cx = a->cx + (b->cx - a->cx) * t;
    o->cy = a->cy + (b->cy - a->cy) * t;
    o->cz = a->cz + (b->cz - a->cz) * t;
    o->cw = a->cw + (b->cw - a->cw) * t;
    o->u  = a->u + (b->u - a->u) * t;
    o->v  = a->v + (b->v - a->v) * t;
    o->fog = a->fog + (b->fog - a->fog) * t;
    o->r = a->r + (int)((float)(b->r - a->r) * t);
    o->g = a->g + (int)((float)(b->g - a->g) * t);
    o->b = a->b + (int)((float)(b->b - a->b) * t);
    o->a = a->a + (int)((float)(b->a - a->a) * t);
    /* oor=2 marks a vertex CREATED by the near clip: PPSSPP's clip_interpolate re-projects
     * such vertices and re-checks the screen-grid range (clip_vtx_out_of_range below). */
    o->oor = 2; o->ozp = o->ozn = 0;
    o->sx = o->sy = o->sz = 0.0f;
}

/* PPSSPP Clipper clip_interpolate + ClipToScreenInternal<.., alwaysCheckRange=true>: a vertex
 * produced by the near-z clip is re-projected, and if its screen position leaves the 4096
 * grid the vertex is poisoned (screenpos = 0x7FFFFFFF) — every sub-triangle that references
 * it is discarded (Clipper.cpp ProcessTriangle line ~406). Without this, clipped fans from
 * geometry crossing the camera rasterize as huge wedges at z≈nearest, which then z-reject
 * everything drawn after them (bright background triangles punching through models). */
static int clip_vtx_out_of_range(CVtx *v) {
    if (v->oor != 2) return 0;        /* original vertex: transform-time verdict already passed */
    if (v->cw <= NEAR_W) return 1;    /* w ~ 0: projection blows up (PPSSPP gets inf coords) */
    float sx = v->cx * decode_float24(ge.viewport_raw[0]) / v->cw + decode_float24(ge.viewport_raw[3]);
    float sy = v->cy * decode_float24(ge.viewport_raw[1]) / v->cw + decode_float24(ge.viewport_raw[4]);
    float sz = v->cz * decode_float24(ge.viewport_raw[2]) / v->cw + decode_float24(ge.viewport_raw[5]);
    v->sx = sx; v->sy = sy; v->sz = sz;
    const float SB = 4095.0f + 15.5f / 16.0f;
    if (ge.depth_clip)
        return sx >= SB || sy >= SB || sx < 0.0f || sy < 0.0f;
    return sx > SB || sy >= SB || sx < 0.0f || sy < 0.0f || sz < 0.0f || sz >= 65536.0f;
}

/* Signed distance to the PSP's only clip plane: z >= -w (ndc z = -1). The PSP clips against
 * negative Z only, regardless of viewport (PPSSPP Clipper CLIP_POLY(CLIP_NEG_Z_BIT,0,0,1,1)). */
static float clip_d_nearz(const CVtx *v) { return v->cz + v->cw; }

/* Sutherland–Hodgman against z >= -w. in[] has n vertices (a triangle);
 * out[] receives up to n+1. Returns the clipped vertex count (0 if fully behind). */
static int clip_poly_near(const CVtx *in, int n, CVtx *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const CVtx *A = &in[i], *B = &in[(i + 1) % n];
        float da = clip_d_nearz(A), db = clip_d_nearz(B);
        int ain = da >= 0.0f, bin = db >= 0.0f;
        if (ain) out[m++] = *A;
        if (ain != bin) {
            float t = da / (da - db);
            clip_lerp_cvtx(A, B, t, &out[m++]);
        }
    }
    return m;
}

static int s_gelog = -1;
static int s_gewatch = -1, s_gewatch_after = -1, s_gewatch_limit = -1;
static int s_ge_arm_rect = -1;
static int s_ge_arm_x0, s_ge_arm_y0, s_ge_arm_x1, s_ge_arm_y1;

/* Bridge a decoded GE primitive back to its guest producer without pausing the
 * runtime. Once a requested through-sprite rectangle is observed, watch both
 * source vertex records for later writes. Repeated addresses are deduplicated by
 * sr_add_mem_watch(); rotating arenas can arm up to the shared watch-table limit. */
static void ge_arm_rect_maybe(int xa, int ya, int xb, int yb,
                              uint32_t va0, uint32_t va1, uint32_t stride,
                              uint32_t xoff, uint32_t xwidth) {
    if (s_ge_arm_rect < 0) {
        const char *spec = getenv("SR_GE_ARM_RECT");
        s_ge_arm_rect = spec &&
            sscanf(spec, "%d,%d,%d,%d", &s_ge_arm_x0, &s_ge_arm_y0,
                   &s_ge_arm_x1, &s_ge_arm_y1) == 4;
        if (spec && !s_ge_arm_rect) {
            fprintf(stderr, "SR_GE_ARM_RECT: expected x0,y0,x1,y1, got '%s'\n", spec);
        }
    }
    if (!s_ge_arm_rect || xa != s_ge_arm_x0 || ya != s_ge_arm_y0 ||
        xb != s_ge_arm_x1 || yb != s_ge_arm_y1) {
        return;
    }
    int before = g_sr_mem_watch_count;
    sr_add_mem_watch(va0, va0 + stride, "ge_rect_v0");
    sr_add_mem_watch(va1, va1 + stride, "ge_rect_v1");
    if (g_sr_mem_watch_count != before) {
        fprintf(stderr,
                "GE_ARM_RECT frame=%u rect=(%d,%d)-(%d,%d) va=0x%08x/0x%08x stride=%u watches=%d\n",
                s_ge_frame, xa, ya, xb, yb, va0, va1, stride, g_sr_mem_watch_count);
    }
    uint32_t wa0 = 0, ww0 = 0, wv0 = 0, wp0 = 0;
    uint32_t wa1 = 0, ww1 = 0, wv1 = 0, wp1 = 0;
    int found0 = sr_find_last_writer(va0 + xoff, xwidth, &wa0, &ww0, &wv0, &wp0);
    int found1 = sr_find_last_writer(va1 + xoff, xwidth, &wa1, &ww1, &wv1, &wp1);
    fprintf(stderr,
            "GE_RECT_WRITER frame=%u xaddr=0x%08x/0x%08x writer="
            "%s0x%08x@0x%08x/%u/%08x,%s0x%08x@0x%08x/%u/%08x\n",
            s_ge_frame, va0 + xoff, va1 + xoff,
            found0 ? "" : "missing:", wp0, wa0, ww0, wv0,
            found1 ? "" : "missing:", wp1, wa1, ww1, wv1);
}

/* SR_GEMAT=1: dump the T&L state and the first vertex's full pipeline for the first 16
 * transform-mode PRIMs — pinpoints where 3D geometry dies (bad matrix vs bad vertex data). */
static void dump_xform_debug(uint32_t vbase, uint32_t ibase, int idxfmt, const VFmt *vf,
                             int type, int count) {
    static int on = -1, n = 0;
    if (on < 0) on = getenv("SR_GEMAT") ? 1 : 0;
    if (!on || n >= 16) return;
    n++;
    fprintf(stderr, "GEMAT frame=%u prim type=%d count=%d vtype=0x%06x vaddr=0x%08x\n",
            s_ge_frame, type, count, ge.vtype, vbase);
    fprintf(stderr, "  world:"); for (int i=0;i<12;i++) fprintf(stderr, " %.5g", ge.world[i]); fprintf(stderr, "\n");
    fprintf(stderr, "  view :"); for (int i=0;i<12;i++) fprintf(stderr, " %.5g", ge.view[i]);  fprintf(stderr, "\n");
    fprintf(stderr, "  proj :"); for (int i=0;i<16;i++) fprintf(stderr, " %.5g", ge.proj[i]);  fprintf(stderr, "\n");
    fprintf(stderr, "  vp   : xs=%.4g ys=%.4g zs=%.4g xc=%.4g yc=%.4g zc=%.4g offx=%.4g offy=%.4g\n",
            decode_float24(ge.viewport_raw[0]), decode_float24(ge.viewport_raw[1]),
            decode_float24(ge.viewport_raw[2]), decode_float24(ge.viewport_raw[3]),
            decode_float24(ge.viewport_raw[4]), decode_float24(ge.viewport_raw[5]),
            (float)(ge.offsetx & 0xFFFF) / 16.0f, (float)(ge.offsety & 0xFFFF) / 16.0f);
    uint32_t a = prim_vtx_addr(vbase, ibase, idxfmt, 0, vf->stride);
    float mx,my,mz, wx,wy,wz, cx,cy,cz, X,Y,Z,W;
    read_pos(a, vf, &mx, &my, &mz);
    mat43_mul(ge.world, mx, my, mz, &wx, &wy, &wz);
    mat43_mul(ge.view,  wx, wy, wz, &cx, &cy, &cz);
    mat44_mul(ge.proj,  cx, cy, cz, &X, &Y, &Z, &W);
    fprintf(stderr, "  v0: model=(%.4g,%.4g,%.4g) world=(%.4g,%.4g,%.4g) view=(%.4g,%.4g,%.4g) clip=(%.4g,%.4g,%.4g) clipw=%.4g%s\n",
            mx,my,mz, wx,wy,wz, cx,cy,cz, X,Y,Z, W, W <= 0.0f ? "  << NEAR-REJECTED" : "");
}

/* Fast clear-mode rectangle fill: when both colour and alpha are written the packed value is
 * constant across the rect, so rows are written directly (no per-pixel state checks). */
static void fill_rect_clear_fast(int xa, int ya, int xb, int yb, int r, int g, int b, int a) {
    int cx1 = ge.scis_x1 > 0 ? ge.scis_x1 : 0,   cy1 = ge.scis_y1 > 0 ? ge.scis_y1 : 0;
    int cx2 = ge.scis_x2 < 479 ? ge.scis_x2 : 479, cy2 = ge.scis_y2 < 271 ? ge.scis_y2 : 271;
    if (xa < cx1) xa = cx1;
    if (xb > cx2 + 1) xb = cx2 + 1;
    if (ya < cy1) ya = cy1;
    if (yb > cy2 + 1) yb = cy2 + 1;
    if (xa >= xb || ya >= yb) return;
    if (s_stat_on > 0 && xa <= 240 && 240 < xb && ya <= 136 && 136 < yb && s_pxc < 32) {
        s_pxc++;
        fprintf(stderr, "PXC f=%u CLEARFILL rect=(%d,%d)-(%d,%d) rgba=(%d,%d,%d,%d) fbp=0x%08x\n",
                s_ge_frame, xa, ya, xb, yb, r, g, b, a, ge.fbp);
    }
    uint32_t c = pack_fb(r, g, b, a);
    uint32_t stride = ge.fbw ? ge.fbw : 512;
    uint32_t fb = ge_fb_addr();
    int wide = (ge.fbfmt & 3) == 3;
    for (int y = ya; y < yb; y++) {
        uint32_t row = fb + (uint32_t)(y * (int)stride) * (wide ? 4u : 2u);
        if (wide) for (int x = xa; x < xb; x++) MEM_W32(row + (uint32_t)x*4, c);
        else      for (int x = xa; x < xb; x++) MEM_W16(row + (uint32_t)x*2, (uint16_t)c);
    }
    g_ge_pixels += (unsigned long)(xb - xa) * (unsigned long)(yb - ya);
}

/* Rasterise a sprite (axis-aligned rect from two corner vertices). Used by both modes;
 * persp=1 recovers UV from the over-w storage and depth-tests with the second vertex's z. */
static void fill_sprite(const Vtx *p0, const Vtx *p1, int persp) {
    if (s_gpu && s_gpu->sprite) {
        uint64_t profile_started = ge_cpu_profile_begin();
        int handled = s_gpu->sprite(p0, p1, persp);
        ge_cpu_profile_end(GE_CPU_GPU_HOOK, profile_started);
        if (handled) return;
    }

    int xa=(int)fminf(p0->x,p1->x), xb=(int)fmaxf(p0->x,p1->x);
    int ya=(int)fminf(p0->y,p1->y), yb=(int)fmaxf(p0->y,p1->y);

    if (ge.clear) {
        /* Clear-mode: bit 10 clears depth (to the sprite's z), bits 8/9 colour/alpha. */
        if (ge.clear_mode & 0x400)
            zbuf_fill_rect(xa, ya, xb - 1, yb - 1, (uint16_t)p1->z);
        who_init();
        if (s_who_x >= xa && s_who_x < xb && s_who_y >= ya && s_who_y < yb && s_pixwho_n < 400) {
            s_pixwho_n++;
            fprintf(stderr, "PIXWHO f=%u CLEARSPR mode=0x%03x z=%.0f rect=(%d,%d)-(%d,%d) rgba=(%d,%d,%d,%d)\n",
                    s_ge_frame, ge.clear_mode, p1->z, xa, ya, xb, yb, p1->r, p1->g, p1->b, p1->a);
        }
        int wc = (ge.clear_mode & 0x100) != 0, wa = (ge.clear_mode & 0x200) != 0;
        if (!wc && !wa) return;
        if (wc && wa) { fill_rect_clear_fast(xa, ya, xb, yb, p1->r, p1->g, p1->b, p1->a); return; }
        for (int y=ya; y<yb; y++) for (int x=xa; x<xb; x++)
            put_px_rgba(x, y, p1->r, p1->g, p1->b, p1->a);
        return;
    }

    float u0=p0->u, v0=p0->v, u1=p1->u, v1=p1->v;
    if (persp) {  /* UV stored over w; recover texel coords */
        u0 = p0->u / (p0->rw != 0.0f ? p0->rw : 1.0f);
        v0 = p0->v / (p0->rw != 0.0f ? p0->rw : 1.0f);
        u1 = p1->u / (p1->rw != 0.0f ? p1->rw : 1.0f);
        v1 = p1->v / (p1->rw != 0.0f ? p1->rw : 1.0f);
    }
    if (!zrange_ok(p1->z, persp)) { if (s_stat_on > 0) s_stat.zrange++; return; }
    uint16_t pz = clamp_z(p1->z);
    int use_z = persp ? ge.ztest_enable : thru_ztest();
    int fog = (persp && ge.fog_enable) ? (int)(clampf01(p1->fog)*256.0f) : 256;
    s_in3d = persp;
    if (persp) s_stat.spr3d++; else s_stat.spr2d++;
    float uw =(xb>xa)?(u1-u0)/(float)(xb-xa):0;
    float uvv=(yb>ya)?(v1-v0)/(float)(yb-ya):0;
    for (int y=ya; y<yb; y++) {
        float fv = v0 + (y-ya)*uvv;
        for (int x=xa; x<xb; x++) {
            float u = u0 + (x-xa)*uw;
            int r,g,b,a;
            if (!shade(u,fv,fog,p1->r,p1->g,p1->b,p1->a,&r,&g,&b,&a)) continue;
            put_px_rgba_tested(x,y,pz,use_z,r,g,b,a);
        }
    }
}

typedef struct { float basis[4], deriv[4]; } PatchWeight;

static void bezier_weight(float t, PatchWeight *w) {
    float s=1.0f-t,s2=s*s,t2=t*t;
    w->basis[0]=s2*s;w->basis[1]=3.0f*t*s2;w->basis[2]=3.0f*t2*s;w->basis[3]=t2*t;
    w->deriv[0]=-3.0f*s2;w->deriv[1]=9.0f*t2-12.0f*t+3.0f;
    w->deriv[2]=3.0f*(2.0f-3.0f*t)*t;w->deriv[3]=3.0f*t2;
}

/* Scalar form of PPSSPP Spline3DWeight::CalcWeights, including open edge knots. */
static void spline_weight(int patch,int tile,int tess,int patches,int type,PatchWeight *w) {
    float d30=1.0f/3.0f,d41=1.0f/3.0f,d52=1.0f/3.0f,d31=0.5f,d42=0.5f,d32=1.0f;
    if(type&1){if(patch==0){d30=1.0f;d41=0.5f;d31=1.0f;}else if(patch==1)d30=0.5f;}
    if(type&2){if(patch==patches-1){d41=0.5f;d52=1.0f;d42=1.0f;}else if(patch==patches-2)d52=0.5f;}
    float knots[3]={(float)patch-2.0f,(float)patch-1.0f,(float)patch};
    if(type&1)for(int i=0;i<3;i++){int k=patch+i;if(k==0||k==1)knots[i]=0.0f;}
    float t=(float)(patch*tess+tile)/(float)tess;
    float t0=t-knots[0],t1=t-knots[1],t2=t-knots[2];
    float f30=t0*d30,f41=t1*d41,f52=t2*d52,f31=t1*d31,f42=t2*d42,f32=t2*d32;
    float a=(1.0f-f30)*(1.0f-f31),b=f31*f41,c=(1.0f-f41)*(1.0f-f42),d=f42*f52;
    w->basis[0]=a*(1.0f-f32);w->basis[1]=1.0f-a-b+(a+b+c-1.0f)*f32;
    w->basis[2]=b+(1.0f-b-c-d)*f32;w->basis[3]=d*f32;
    float i1=(1.0f-f31)*(1.0f-f32),i2=f31*(1.0f-f32)+(1.0f-f42)*f32,i3=f42*f32;
    float f130=i1*d30,f241=i2*d41,f352=i3*d52;
    w->deriv[0]=-3.0f*f130;w->deriv[1]=3.0f*(f130-f241);
    w->deriv[2]=3.0f*(f241-f352);w->deriv[3]=3.0f*f352;
}

static void patch_eval(const ModelVtx *cp,int row_stride,const PatchWeight *wu,
                       const PatchWeight *wv,int gen_uv,float gu,float gv,ModelVtx *m) {
    float p[7]={0},du[3]={0},dv[3]={0};memset(m,0,sizeof(*m));
    for(int v=0;v<4;v++)for(int u=0;u<4;u++){
        const ModelVtx *q=&cp[v*row_stride+u];float k=wu->basis[u]*wv->basis[v];
        p[0]+=q->x*k;p[1]+=q->y*k;p[2]+=q->z*k;p[3]+=(float)q->r*k;
        p[4]+=(float)q->g*k;p[5]+=(float)q->b*k;p[6]+=(float)q->a*k;
        if(!gen_uv){m->u+=q->u*k;m->v+=q->v*k;}
        float ku=wu->deriv[u]*wv->basis[v],kv=wu->basis[u]*wv->deriv[v];
        du[0]+=q->x*ku;du[1]+=q->y*ku;du[2]+=q->z*ku;
        dv[0]+=q->x*kv;dv[1]+=q->y*kv;dv[2]+=q->z*kv;
    }
    m->x=p[0];m->y=p[1];m->z=p[2];m->r=clamp8((int)p[3]);m->g=clamp8((int)p[4]);
    m->b=clamp8((int)p[5]);m->a=clamp8((int)p[6]);if(gen_uv){m->u=gu;m->v=gv;}
    m->nx=du[1]*dv[2]-du[2]*dv[1];m->ny=du[2]*dv[0]-du[0]*dv[2];m->nz=du[0]*dv[1]-du[1]*dv[0];
    float nl=sqrtf(m->nx*m->nx+m->ny*m->ny+m->nz*m->nz);
    if(nl>0.0f){m->nx/=nl;m->ny/=nl;m->nz/=nl;}else{m->nx=0;m->ny=0;m->nz=1;}
    if(ge.patch_facing&1){m->nx=-m->nx;m->ny=-m->ny;m->nz=-m->nz;}
}

static void model_to_through(const ModelVtx *m,Vtx *v){
    v->x=m->x;v->y=m->y;v->z=m->z;v->rw=1;v->u=m->u;v->v=m->v;v->fog=1;
    v->r=m->r;v->g=m->g;v->b=m->b;v->a=m->a;
}

static void submit_model_triangle(const ModelVtx *a,const ModelVtx *b,const ModelVtx *c,int through){
    if(through){Vtx x,y,z;model_to_through(a,&x);model_to_through(b,&y);model_to_through(c,&z);raster_tri(&x,&y,&z,0);return;}
    CVtx cv[3],cl[4];transform_model_vtx_clip(a,&cv[0]);transform_model_vtx_clip(b,&cv[1]);transform_model_vtx_clip(c,&cv[2]);
    if(ge.cull_enable&&!ge.clear&&(ge.cull&1)==0){CVtx t=cv[0];cv[0]=cv[1];cv[1]=t;}
    if(cv[0].oor||cv[1].oor||cv[2].oor||(cv[0].cw<0&&cv[1].cw<0&&cv[2].cw<0))return;
    int zp=cv[0].ozp+cv[1].ozp+cv[2].ozp,zn=cv[0].ozn+cv[1].ozn+cv[2].ozn;
    if((!ge.depth_clip&&zp+zn>0)||zp>=3||zn>=3)return;
    if(clip_d_nearz(&cv[0])>=0&&clip_d_nearz(&cv[1])>=0&&clip_d_nearz(&cv[2])>=0){
        if(cv[0].cw<=NEAR_W||cv[1].cw<=NEAR_W||cv[2].cw<=NEAR_W)return;
        Vtx x,y,z;project_cvtx(&cv[0],&x);project_cvtx(&cv[1],&y);project_cvtx(&cv[2],&z);raster_tri(&x,&y,&z,1);
    }else{
        int n=clip_poly_near(cv,3,cl);Vtx p[4];int bad[4]={1,1,1,1};
        for(int i=0;i<n&&i<4;i++){bad[i]=cl[i].cw<=NEAR_W||clip_vtx_out_of_range(&cl[i]);if(!bad[i])project_cvtx(&cl[i],&p[i]);}
        for(int i=2;i<n&&i<4;i++)if(!bad[0]&&!bad[i-1]&&!bad[i])raster_tri(&p[0],&p[i-1],&p[i],1);
    }
}

static void submit_model_line(const ModelVtx *a,const ModelVtx *b,int through){
    if(through){Vtx x,y;model_to_through(a,&x);model_to_through(b,&y);draw_line_vtx(&x,&y,0);return;}
    CVtx x,y;transform_model_vtx_clip(a,&x);transform_model_vtx_clip(b,&y);
    if(x.oor||y.oor||(x.cw<0&&y.cw<0))return;
    float dx=clip_d_nearz(&x),dy=clip_d_nearz(&y);
    if(dx<0&&dy<0)return;
    if(dx<0){CVtx t;clip_lerp_cvtx(&x,&y,dx/(dx-dy),&t);x=t;}
    else if(dy<0){CVtx t;clip_lerp_cvtx(&y,&x,dy/(dy-dx),&t);y=t;}
    if(x.cw<=NEAR_W||y.cw<=NEAR_W||clip_vtx_out_of_range(&x)||clip_vtx_out_of_range(&y))return;
    Vtx vx,vy;project_cvtx(&x,&vx);project_cvtx(&y,&vy);draw_line_vtx(&vx,&vy,1);
}

static void submit_model_point(const ModelVtx *a,int through){
    if(through){Vtx v;model_to_through(a,&v);draw_point_vtx(&v,0);return;}
    CVtx c;transform_model_vtx_clip(a,&c);if(c.cw<=NEAR_W||c.oor||(!ge.depth_clip&&(c.ozp||c.ozn)))return;
    Vtx v;project_cvtx(&c,&v);draw_point_vtx(&v,1);
}

static void draw_patch(uint32_t op,int spline){
    int nu=(int)(op&255),nv=(int)((op>>8)&255);
    VFmt vf;decode_vtype(ge.vtype,&vf);int idxfmt=(int)((ge.vtype>>11)&3),count=nu*nv;
    uint32_t vbase=ge.vaddr,ibase=ge.iaddr;
    if(idxfmt)ge.iaddr+=(uint32_t)(count<<(idxfmt-1));else ge.vaddr+=(uint32_t)(count*vf.stride);
    if(nu<4||nv<4)return;
    ModelVtx *all=(ModelVtx*)malloc((size_t)count*sizeof(*all));if(!all)return;
    for(int i=0;i<count;i++)decode_model_vtx(prim_vtx_addr(vbase,ibase,idxfmt,i,vf.stride),&vf,&all[i]);
    int pu=spline?nu-3:(nu-1)/3,pv=spline?nv-3:(nv-1)/3;
    int tu=(int)(ge.patch_division&0x7F),tv=(int)((ge.patch_division>>8)&0x7F);if(tu<1)tu=1;if(tv<1)tv=1;
    for(;;){uint64_t n=spline?(uint64_t)(pu*tu+1)*(uint64_t)(pv*tv+1):(uint64_t)(tu+1)*(tv+1)*pu*pv;
        if(n<=65535u||(tu==1&&tv==1))break;
        if(tu>1)tu--;
        if(tv>1)tv--;
    }
    int prim=(int)(ge.patch_primitive&3),typeu=(int)((op>>16)&3),typev=(int)((op>>18)&3);
    for(int py=0;py<pv;py++)for(int px=0;px<pu;px++){
        int base=(spline?py:py*3)*nu+(spline?px:px*3);
        for(int y=0;y<tv;y++)for(int x=0;x<tu;x++){
            PatchWeight ux[2],vy[2];
            if(spline){spline_weight(px,x,tu,pu,typeu,&ux[0]);spline_weight(px,x+1,tu,pu,typeu,&ux[1]);
                spline_weight(py,y,tv,pv,typev,&vy[0]);spline_weight(py,y+1,tv,pv,typev,&vy[1]);}
            else{bezier_weight((float)x/tu,&ux[0]);bezier_weight((float)(x+1)/tu,&ux[1]);
                bezier_weight((float)y/tv,&vy[0]);bezier_weight((float)(y+1)/tv,&vy[1]);}
            ModelVtx q[4];int gen=vf.tc_off<0;
            patch_eval(all+base,nu,&ux[0],&vy[0],gen,(float)px+(float)x/tu,(float)py+(float)y/tv,&q[0]);
            patch_eval(all+base,nu,&ux[1],&vy[0],gen,(float)px+(float)(x+1)/tu,(float)py+(float)y/tv,&q[1]);
            patch_eval(all+base,nu,&ux[0],&vy[1],gen,(float)px+(float)x/tu,(float)py+(float)(y+1)/tv,&q[2]);
            patch_eval(all+base,nu,&ux[1],&vy[1],gen,(float)px+(float)(x+1)/tu,(float)py+(float)(y+1)/tv,&q[3]);
            if(prim==0){submit_model_triangle(&q[0],&q[2],&q[1],vf.through);submit_model_triangle(&q[1],&q[2],&q[3],vf.through);}
            else if(prim==1){submit_model_line(&q[0],&q[2],vf.through);submit_model_line(&q[1],&q[3],vf.through);submit_model_line(&q[1],&q[2],vf.through);}
            else submit_model_point(&q[0],vf.through);
        }
    }
    free(all);
}

/* When an entire 3D primitive lands behind the near plane (all clip-w <= NEAR_W), dump the raw
 * model-space position, the resulting w's, and the full matrix state -- distinguishes "camera
 * genuinely faces away" from "our matrix state is wrong for this draw". Budgeted per stat window
 * (reset alongside the other SR_GESTAT trace budgets). */
static void near_dump(const char *what, uint32_t a0, const VFmt *vf, const CVtx *cv, int n) {
    if (s_stat_on <= 0 || s_neardump >= 3) return;
    s_neardump++;
    float mx, my, mz; read_pos(a0, vf, &mx, &my, &mz);
    fprintf(stderr, "NEARDUMP f=%u %s vtype=0x%06x raw=(%g,%g,%g) cw=(", s_ge_frame, what, ge.vtype, mx, my, mz);
    for (int i = 0; i < n; i++) fprintf(stderr, "%s%g", i ? "," : "", cv[i].cw);
    fprintf(stderr, ")\n  world=[%g %g %g; %g %g %g; %g %g %g; %g %g %g]\n",
            ge.world[0], ge.world[1], ge.world[2], ge.world[3], ge.world[4], ge.world[5],
            ge.world[6], ge.world[7], ge.world[8], ge.world[9], ge.world[10], ge.world[11]);
    fprintf(stderr, "  view =[%g %g %g; %g %g %g; %g %g %g; %g %g %g]\n",
            ge.view[0], ge.view[1], ge.view[2], ge.view[3], ge.view[4], ge.view[5],
            ge.view[6], ge.view[7], ge.view[8], ge.view[9], ge.view[10], ge.view[11]);
    fprintf(stderr, "  proj =[%g %g %g %g; %g %g %g %g; %g %g %g %g; %g %g %g %g]\n",
            ge.proj[0], ge.proj[1], ge.proj[2], ge.proj[3], ge.proj[4], ge.proj[5],
            ge.proj[6], ge.proj[7], ge.proj[8], ge.proj[9], ge.proj[10], ge.proj[11],
            ge.proj[12], ge.proj[13], ge.proj[14], ge.proj[15]);
}

uint32_t g_frame_prims = 0;
/* Backend-independent, per-list primitive classification for bounded GE
 * provenance traces.  Count at the command frontend before either the Vulkan
 * hook or software rasterizer can accept/drop the draw. */
unsigned long g_ge_list_through_cmds = 0, g_ge_list_transform_cmds = 0;
unsigned long g_ge_list_through_vertices = 0, g_ge_list_transform_vertices = 0;
unsigned long g_ge_list_through_sprites = 0, g_ge_list_transform_sprites = 0;
static int s_strip_cache = -1;

static int strip_cache_enabled(void) {
    if (s_strip_cache < 0) {
        const char *disable = getenv("SR_GE_STRIP_CACHE_DISABLE");
        s_strip_cache = !(disable && disable[0] && strcmp(disable, "0") != 0);
    }
    return s_strip_cache;
}

/* Account the sparse population once per primitive command.  Keeping these
 * totals out of prim_vtx_addr() leaves the hot path with only the sample
 * ordinal/mask bookkeeping; the formulas mirror the existing coarse profile. */
static void primitive_profile_note_draw(int type, int count, const VFmt *vf) {
    if (!s_primitive_profile) return;
    s_cpu_profile_stats.primitive_profile_commands[type]++;

    uint64_t uses = 0;
    uint64_t vertex_references = 0;
    uint64_t submitted = 0;
    if (type == 0) uses = vertex_references = (uint64_t)count;
    else if (type == 1) uses = vertex_references = (uint64_t)(count / 2) * 2u;
    else if (type == 2) uses = vertex_references = count > 0 ? (uint64_t)(count - 1) * 2u : 0;
    else if (type == 3) uses = vertex_references = (uint64_t)(count / 3) * 3u;
    else if (type == 4) {
        uint64_t legacy_uses = count > 2 ? (uint64_t)(count - 2) * 3u : 0;
        vertex_references = legacy_uses;
        int cache_strip = strip_cache_enabled() && count >= 3;
        uses = cache_strip ? (uint64_t)count : legacy_uses;
        if (cache_strip) {
            s_cpu_profile_stats.primitive_profile_strip_cache_commands++;
            s_cpu_profile_stats.primitive_profile_strip_cache_hits += legacy_uses - uses;
        }
    } else if (type == 5) uses = vertex_references = count > 2 ? (uint64_t)(count - 2) * 3u : 0;
    else if (type == 6) uses = vertex_references = (uint64_t)(count / 2) * 2u;

    if (type == 0) submitted = (uint64_t)count;
    else if (type == 1) submitted = (uint64_t)(count / 2);
    else if (type == 2) submitted = count > 0 ? (uint64_t)(count - 1) : 0;
    else if (type == 3) submitted = (uint64_t)count / 3u;
    else if (type == 4 || type == 5) submitted = count > 2 ? (uint64_t)count - 2u : 0;
    else if (type == 6) submitted = (uint64_t)(count / 2);
    s_cpu_profile_stats.primitive_profile_submitted[type] += submitted;
    if (type < 3 || type == 6)
        s_cpu_profile_stats.primitive_profile_non_triangle_primitives += submitted;

    s_cpu_profile_stats.primitive_profile_vertex_references += vertex_references;
    if (type == 3 || type == 4 || type == 5)
        s_cpu_profile_stats.primitive_profile_triangle_vertex_references += vertex_references;
    else
        s_cpu_profile_stats.primitive_profile_non_triangle_vertex_references += vertex_references;

    s_cpu_profile_stats.primitive_profile_vertices += uses;
    s_cpu_profile_stats.primitive_profile_eligible[GE_PRIM_PROFILE_VERTEX_FETCH_DECODE] += uses;
    if (vf->through) {
        s_cpu_profile_stats.primitive_profile_through_vertex_uses += uses;
    } else {
        s_cpu_profile_stats.primitive_profile_transform_vertices += uses;
        s_cpu_profile_stats.primitive_profile_transform_vertex_uses += uses;
        s_cpu_profile_stats.primitive_profile_eligible[GE_PRIM_PROFILE_TRANSFORM] += uses;
        if (ge.lighting_enable)
            s_cpu_profile_stats.primitive_profile_eligible[GE_PRIM_PROFILE_LIGHTING] += uses;
    }

    uint64_t triangles = 0;
    if (type == 3) triangles = (uint64_t)count / 3u;
    else if (type == 4 || type == 5) triangles = count > 2 ? (uint64_t)count - 2u : 0;
    if (type == 3 || type == 4 || type == 5)
        s_cpu_profile_stats.primitive_profile_triangle_vertex_uses += uses;
    else
        s_cpu_profile_stats.primitive_profile_non_triangle_vertex_uses += uses;
    s_cpu_profile_stats.primitive_profile_triangle_candidates += triangles;
    if (vf->through)
        s_cpu_profile_stats.primitive_profile_through_triangle_candidates += triangles;
    else
        s_cpu_profile_stats.primitive_profile_transform_triangle_candidates += triangles;
    s_cpu_profile_stats.primitive_profile_eligible[GE_PRIM_PROFILE_ASSEMBLY] += triangles;
    if (!vf->through)
        s_cpu_profile_stats.primitive_profile_eligible[GE_PRIM_PROFILE_CLIPPING_ACCEPTANCE] += triangles;
}

static void draw_prim(uint32_t op, unsigned long prim_index) {
    g_frame_prims++;
    s_primitive_profile_counting = 0;
    primitive_profile_clear_pending();
    int type = (op >> 16) & 7;
    int count = op & 0xFFFF;
    if (s_cpu_profile) {
        uint64_t uses = 0;
        if (type == 0) uses = (uint64_t)count;
        else if (type == 1) uses = (uint64_t)(count / 2) * 2u;
        else if (type == 2) uses = count > 0 ? (uint64_t)(count - 1) * 2u : 0;
        else if (type == 3) uses = (uint64_t)(count / 3) * 3u;
        else if (type == 4) {
            uint64_t legacy_uses = count > 2 ? (uint64_t)(count - 2) * 3u : 0;
            if (strip_cache_enabled() && count >= 3) {
                uses = (uint64_t)count;
                s_cpu_profile_stats.strip_vertices_reused += legacy_uses - uses;
            } else {
                uses = legacy_uses;
            }
        }
        else if (type == 5) uses = count > 2 ? (uint64_t)(count - 2) * 3u : 0;
        else if (type == 6) uses = (uint64_t)(count / 2) * 2u;
        s_cpu_profile_stats.vertex_decode_uses += uses;
        s_cpu_profile_stats.primitive_type[type]++;
    }
    VFmt vf; decode_vtype(ge.vtype, &vf);
    primitive_profile_note_draw(type, count, &vf);
    if (vf.through) {
        g_ge_list_through_cmds++;
        g_ge_list_through_vertices += (unsigned long)count;
        if (type == 6) g_ge_list_through_sprites += (unsigned long)(count / 2);
    } else {
        g_ge_list_transform_cmds++;
        g_ge_list_transform_vertices += (unsigned long)count;
        if (type == 6) g_ge_list_transform_sprites += (unsigned long)(count / 2);
    }
    if (s_gelog < 0) s_gelog = getenv("SR_GEDUMP") ? 1 : 0;
    if (s_gewatch < 0) s_gewatch = getenv("SR_GEWATCH") ? 1 : 0;
    if (s_gewatch_after < 0) { const char *p=getenv("SR_GEWATCH_AFTER"); s_gewatch_after=p?atoi(p):0; }
    if (s_gewatch_limit < 0) { const char *p=getenv("SR_GEWATCH_LIMIT"); s_gewatch_limit=p?atoi(p):240; if(s_gewatch_limit<=0)s_gewatch_limit=240; }
    int watch_now = s_gewatch && s_ge_frame >= (uint32_t)s_gewatch_after;
    int idxfmt = (ge.vtype >> 11) & 3;
    if (s_gelog) {
        static int n=0, nthrough=0, ntransform=0;
        if (vf.through) nthrough++; else ntransform++;
        if (n++ < 40 || (!vf.through && ntransform < 20))
            fprintf(stderr, "GE PRIM type=%d count=%d vtype=0x%06x through=%d idx=%d tex=%d fmt=%u fbp=0x%08x vaddr=0x%08x iaddr=0x%08x [thru=%d xform=%d]\n",
                type, count, ge.vtype, vf.through, idxfmt, ge.tex_enable, ge.tex_fmt, ge.fbp, ge.vaddr, ge.iaddr, nthrough, ntransform);
    }
    uint32_t vbase = ge.vaddr, ibase = ge.iaddr;
    /* Advance vertex/index pointer (PPSSPP GPUCommon::AdvanceVerts). */
    if (idxfmt) ge.iaddr += (uint32_t)(count << (idxfmt - 1));
    else        ge.vaddr += (uint32_t)(count * vf.stride);

    /* Observe both the through and transform frontends.  The optional frame gate
     * preserves the fixed distinct-texture budget for late deterministic scenes. */
    texdump_maybe();

    /* ---- THROUGH-MODE (2D, screen coords) ---- */
    s_primitive_profile_through = vf.through;
    s_primitive_profile_counting = s_primitive_profile;
    if (vf.through) {
        if (type == 0) {           /* points */
            for (int i = 0; i < count; i++) {
                Vtx a; load_vtx(prim_vtx_addr(vbase,ibase,idxfmt,i,vf.stride),&vf,&a);
                draw_point_vtx(&a, 0);
            }
        } else if (type == 1 || type == 2) {   /* lines / line strip */
            for (int i = 1; i < count; i++) {
                if (type == 1 && (i & 1) == 0) continue;
                Vtx a, b;
                load_vtx(prim_vtx_addr(vbase,ibase,idxfmt,i-1,vf.stride),&vf,&a);
                load_vtx(prim_vtx_addr(vbase,ibase,idxfmt,i,  vf.stride),&vf,&b);
                draw_line_vtx(&a,&b,0);
            }
        } else if (type == 3 || type == 4 || type == 5) {
            int cache_strip = type == 4 && strip_cache_enabled() && count >= 3;
            Vtx strip_prev[2];
            if (cache_strip) {
                load_vtx(prim_vtx_addr(vbase,ibase,idxfmt,0,vf.stride),&vf,&strip_prev[0]);
                load_vtx(prim_vtx_addr(vbase,ibase,idxfmt,1,vf.stride),&vf,&strip_prev[1]);
            }
            for (int i = 2; i < count; i++) {
                int i0, i1, i2;
                if      (type == 3) { if (i%3!=2) continue; i0=i-2; i1=i-1; i2=i; }
                else if (type == 4) { i0=i-2; i1=i-1; i2=i; }
                else                { i0=0;   i1=i-1; i2=i; }
                int tri_profile_slot = primitive_profile_claim_triangle();
                Vtx a, b, c;
                if (cache_strip) {
                    a = strip_prev[0]; b = strip_prev[1];
                    load_vtx(prim_vtx_addr(vbase,ibase,idxfmt,i2,vf.stride),&vf,&c);
                    strip_prev[0] = strip_prev[1]; strip_prev[1] = c;
                } else {
                    load_vtx(prim_vtx_addr(vbase,ibase,idxfmt,i0,vf.stride),&vf,&a);
                    load_vtx(prim_vtx_addr(vbase,ibase,idxfmt,i1,vf.stride),&vf,&b);
                    load_vtx(prim_vtx_addr(vbase,ibase,idxfmt,i2,vf.stride),&vf,&c);
                }
                uint64_t assembly_started = tri_profile_slot == 1 ? SDL_GetTicksNS() : 0;
                /* Same submission-order culling as transform mode (the GE culls in through
                 * mode too); strips alternate, provoking vertex stays last. */
                if (ge.cull_enable && !ge.clear) {
                    int flip = ((ge.cull & 1) == 0) ^ ((type == 4) ? (i & 1) : 0);
                    if (flip) { Vtx t = a; a = b; b = t; }
                }
                if (assembly_started)
                    primitive_profile_end(GE_PRIM_PROFILE_ASSEMBLY, assembly_started);
                primitive_profile_finish_total();
                raster_tri(&a,&b,&c,0);
            }
        } else if (type == 6) {   /* sprites */
            for (int i = 1; i < count; i += 2) {
                Vtx p0, p1;
                uint32_t va0 = prim_vtx_addr(vbase,ibase,idxfmt,i-1,vf.stride);
                uint32_t va1 = prim_vtx_addr(vbase,ibase,idxfmt,i,  vf.stride);
                load_vtx(va0,&vf,&p0);
                load_vtx(va1,&vf,&p1);
                if (watch_now) {
                    int xa=(int)fminf(p0.x,p1.x), xb=(int)fmaxf(p0.x,p1.x);
                    int ya=(int)fminf(p0.y,p1.y), yb=(int)fmaxf(p0.y,p1.y);
                    int area=(xb-xa)*(yb-ya);
                    if (idxfmt||area>=4096) {
                        static int wn=0;
                        if (wn++<s_gewatch_limit)
                            fprintf(stderr,"GEWATCH frame=%u prim=%lu spr idx=%d va=0x%08x/0x%08x area=%d rect=(%d,%d)-(%d,%d) uv=(%.0f,%.0f)-(%.0f,%.0f) color0=%02x%02x%02x%02x color1=%02x%02x%02x%02x tex=%d/0x%08x %dx%d fmt=%u func=0x%06x blend=%d mode=0x%06x fix=%06x/%06x mat=%06x a=%d clear=0x%03x fbp=0x%08x vtype=0x%06x off=(%.4g,%.4g)\n",
                                s_ge_frame,prim_index-1,idxfmt,va0,va1,area,xa,ya,xb,yb,p0.u,p0.v,p1.u,p1.v,
                                p0.a,p0.b,p0.g,p0.r,p1.a,p1.b,p1.g,p1.r,
                                ge.tex_enable,ge.tex_addr,ge.tex_w,ge.tex_h,ge.tex_fmt,ge.tex_func,
                                ge.blend_enable,ge.blend_mode,ge.blend_fixa,ge.blend_fixb,
                                ge.material,ge.material_alpha_set?ge.material_alpha:-1,ge.clear_mode,
                                ge.fbp,ge.vtype,
                                (float)(ge.offsetx&0xFFFF)/16.0f,
                                (float)(ge.offsety&0xFFFF)/16.0f);
                    }
                }
                {
                    int xa=(int)fminf(p0.x,p1.x), xb=(int)fmaxf(p0.x,p1.x);
                    int ya=(int)fminf(p0.y,p1.y), yb=(int)fmaxf(p0.y,p1.y);
                    uint32_t xwidth = vf.pos_fmt == 3 ? 4u :
                                      (vf.pos_fmt == 2 ? 2u : 1u);
                    ge_arm_rect_maybe(xa,ya,xb,yb,va0,va1,
                                      (uint32_t)vf.stride,
                                      (uint32_t)vf.pos_off, xwidth);
                }
                if (s_gelog&&(ge.tex_enable&&!ge.clear)) { static int sn=0; if(sn++<12)
                    fprintf(stderr,"  texspr (%.0f,%.0f)-(%.0f,%.0f) uv(%.0f,%.0f)-(%.0f,%.0f) tex=0x%08x %dx%d fmt=%u bufw=%u\n",
                        p0.x,p0.y,p1.x,p1.y,p0.u,p0.v,p1.u,p1.v,ge.tex_addr,ge.tex_w,ge.tex_h,ge.tex_fmt,ge.tex_bufw); }
                fill_sprite(&p0,&p1,0);
            }
        }
        s_primitive_profile_counting = 0;
        primitive_profile_clear_pending();
        return;
    }

    /* ---- TRANSFORM-MODE (3D, full T&L) ---- */
    /* Diagnostics below may inspect vertices without submitting them; keep
     * those reads out of the sparse event population. */
    s_primitive_profile_counting = 0;
    primitive_profile_clear_pending();
    dump_xform_debug(vbase, ibase, idxfmt, &vf, type, count);
    if (s_rtrace < 0) {
        s_rtrace = getenv("SR_RTRACE") ? 1 : 0;
        const char *rf = getenv("SR_RTRACE_FRAMES");
        if (rf) { s_rt_fbudget = atoi(rf); if (s_rt_fbudget <= 0) s_rt_fbudget = 2; }
    }
    int rt = 0;
    if (s_rtrace) {
        if (s_ge_frame != s_rt_cur_frame) {
            s_rt_cur_frame = s_ge_frame;
            s_rt_active = s_rt_frames < s_rt_fbudget;
            if (s_rt_active) {
                s_rt_frames++;
                s_rt_draws_f = 0;
                who_init();
                fprintf(stderr, "RTRACE f=%u BEGIN fbp=0x%08x/%u fmt=%u zbp=0x%08x dclip=%u probe=(%d,%d)\n",
                        s_ge_frame, ge.fbp, ge.fbw, ge.fbfmt, ge.zbp, ge.depth_clip, s_who_x, s_who_y);
            }
        }
        rt = s_rt_active && s_rt_draws_f < 4000;
        if (rt) s_rt_draws_f++;
    }
    s_rt_draw++;
    if (s_stat_on > 0 && ((ge.vtype >> 9) & 3)) s_stat.skin++;   /* weighted (count field is n-1) */
    /* One line per DISTINCT transform-mode draw signature per stat window: enumerates every
     * unique 3D draw in a scene (type/count/vtype/texture), not just the first few calls. */
    if (s_stat_on > 0 && s_ge3d < 48) {
        uint32_t sig = (uint32_t)type ^ ((uint32_t)count << 3) ^ (ge.vtype << 13) ^ ge.tex_addr ^ (ge.tex_enable ? 0x80000000u : 0);
        int seen = 0;
        for (int si = 0; si < s_ge3d; si++) if (s_ge3d_sigs[si] == sig) { seen = 1; break; }
        if (!seen) {
            s_ge3d_sigs[s_ge3d++] = sig;
            fprintf(stderr, "GE3D f=%u type=%d count=%d vtype=0x%06x tex=%d/0x%08x fmt=%u lit=%d atest=%d/0x%06x blend=%d/%06x world_t=(%.4g,%.4g,%.4g) fog=%d/%g/%g/0x%06x\n",
                    s_ge_frame, type, count, ge.vtype, ge.tex_enable, ge.tex_addr, ge.tex_fmt,
                    ge.lighting_enable, ge.atest_enable, ge.atest, ge.blend_enable, ge.blend_mode,
                    ge.world[9], ge.world[10], ge.world[11],
                    ge.fog_enable, ge.fog_end, ge.fog_slope, ge.fog_color);
            if (ge.lighting_enable)
                fprintf(stderr, "GE3D+ mupd=%d amb=0x%06x/%d mat_a=0x%06x mat_d=0x%06x mat_e=0x%06x texfunc=0x%06x L0=%d/0x%03x ac=0x%06x dc=0x%06x pos=(%.3g,%.3g,%.3g) L1=%d L2=%d L3=%d\n",
                        ge.mat_update, ge.amb_color, ge.amb_alpha, ge.material, ge.mat_diffuse,
                        ge.mat_emissive, ge.tex_func, ge.light_on[0], ge.light_type[0],
                        ge.light_acol[0], ge.light_dcol[0],
                        ge.light_pos[0][0], ge.light_pos[0][1], ge.light_pos[0][2],
                        ge.light_on[1], ge.light_on[2], ge.light_on[3]);
        }
    }
    if (watch_now) {
        static int xn=0;
        if (xn++<s_gewatch_limit)
            fprintf(stderr,"GEWATCH frame=%u xform type=%d count=%d vtype=0x%06x idx=%d vaddr=0x%08x iaddr=0x%08x\n",
                s_ge_frame,type,count,ge.vtype,idxfmt,vbase,ibase);
    }

    /* SR_VDUMP=1: for the first 2 transform-mode draws of each distinct vtype, dump the raw
     * vertex bytes and every decoded attribute plus the post-T&L color, so shading bugs can be
     * separated into decode vs. transform vs. lighting without a debugger attached. */
    {
        static int vdump = -1;
        if (vdump < 0) vdump = getenv("SR_VDUMP") ? 1 : 0;
        if (vdump) {
            static uint32_t seen_vt[16]; static int seen_n[16]; static int seen_cnt = 0;
            /* Re-arm once per 60-frame stat window so later scenes (hangar aircraft, mission
             * terrain) get vertex dumps too, not just the first menu that used the vtype. */
            static uint32_t vd_win = 0;
            if (s_ge_frame / 60 != vd_win) {
                vd_win = s_ge_frame / 60;
                for (int si = 0; si < seen_cnt; si++) seen_n[si] = 0;
            }
            int slot = -1;
            for (int si = 0; si < seen_cnt; si++) if (seen_vt[si] == ge.vtype) { slot = si; break; }
            if (slot < 0 && seen_cnt < 16) { slot = seen_cnt++; seen_vt[slot] = ge.vtype; seen_n[slot] = 0; }
            if (slot >= 0 && seen_n[slot] < 2) {
                seen_n[slot]++;
                fprintf(stderr, "VDUMP f=%u type=%d count=%d vtype=0x%06x stride=%d offs[w=%d tc=%d col=%d nrm=%d pos=%d] lit=%d gouraud=%d fog=%d tmm=0x%03x\n",
                        s_ge_frame, type, count, ge.vtype, vf.stride, vf.w_n ? vf.w_off : -1,
                        vf.tc_off, vf.col_off, vf.nrm_off, vf.pos_off,
                        ge.lighting_enable, ge.shade_gouraud, ge.fog_enable, ge.tex_map_mode);
                if (vf.w_n)
                    fprintf(stderr, "  bone0=[%.3g %.3g %.3g; %.3g %.3g %.3g; %.3g %.3g %.3g; %.3g %.3g %.3g]\n",
                            ge.bone[0], ge.bone[1], ge.bone[2], ge.bone[3], ge.bone[4], ge.bone[5],
                            ge.bone[6], ge.bone[7], ge.bone[8], ge.bone[9], ge.bone[10], ge.bone[11]);
                int nv = count < 6 ? count : 6;
                for (int i = 0; i < nv; i++) {
                    uint32_t a = prim_vtx_addr(vbase, ibase, idxfmt, i, vf.stride);
                    char hex[3*32 + 1]; int hn = vf.stride < 32 ? vf.stride : 32;
                    for (int k = 0; k < hn; k++) sprintf(hex + 3*k, "%02x ", MEM_R8(a + (uint32_t)k));
                    hex[3*hn] = 0;
                    float px, py, pz, nx = 0, ny = 0, nz = 0, u = 0, tv = 0, w0 = 0;
                    int r, g, b, al;
                    read_pos(a, &vf, &px, &py, &pz);
                    read_color(a, &vf, &r, &g, &b, &al);
                    if (vf.nrm_off >= 0) read_nrm(a, &vf, &nx, &ny, &nz);
                    if (vf.tc_off >= 0) read_tc(a, &vf, &u, &tv);
                    if (vf.w_n) { float ws[8]; read_weights(a, &vf, ws); w0 = ws[0]; }
                    CVtx cv2; transform_vtx_clip(a, &vf, &cv2);
                    fprintf(stderr, "  v%d @0x%08x [%s] pos=(%.4g,%.4g,%.4g) uv=(%.3g,%.3g) col=%02x%02x%02x%02x nrm=(%.3g,%.3g,%.3g) w0=%.3g -> cw=%.4g lit=%02x%02x%02x%02x uvT=(%.3g,%.3g) fog=%.3g\n",
                            i, a, hex, px, py, pz, u, tv, r, g, b, al, nx, ny, nz, w0,
                            cv2.cw, cv2.r, cv2.g, cv2.b, cv2.a, cv2.u, cv2.v, cv2.fog);
                }
            }
        }
    }

    s_primitive_profile_counting = s_primitive_profile;
    if (type == 0) {           /* points */
        for (int i = 0; i < count; i++) {
            Vtx a;
            if (!transform_vtx_3d(prim_vtx_addr(vbase,ibase,idxfmt,i,vf.stride),&vf,&a)) continue;
            draw_point_vtx(&a, 1);
        }
    } else if (type == 1 || type == 2) {   /* lines / line strip */
        if (s_stat_on > 0) s_stat.line3d += (unsigned long)(count > 0 ? count - 1 : 0);
        for (int i = 1; i < count; i++) {
            if (type == 1 && (i & 1) == 0) continue;
            CVtx ca, cb;
            transform_vtx_clip(prim_vtx_addr(vbase,ibase,idxfmt,i-1,vf.stride),&vf,&ca);
            transform_vtx_clip(prim_vtx_addr(vbase,ibase,idxfmt,i,  vf.stride),&vf,&cb);
            /* PSP line acceptance (PPSSPP Clipper::ProcessLine): same drop rules as triangles. */
            if (ca.oor || cb.oor) { s_stat.nearclip++; continue; }
            if (ca.cw < 0.0f && cb.cw < 0.0f) { s_stat.nearclip++; continue; }
            if (!ge.depth_clip && (ca.ozp + ca.ozn + cb.ozp + cb.ozn) > 0) { s_stat.nearclip++; continue; }
            if ((ca.ozp + cb.ozp) >= 2 || (ca.ozn + cb.ozn) >= 2) { s_stat.nearclip++; continue; }
            float da = clip_d_nearz(&ca), db2 = clip_d_nearz(&cb);
            if (da < 0.0f && db2 < 0.0f) {
                s_stat.nearclip++;
                CVtx both[2]; both[0] = ca; both[1] = cb;
                s_primitive_profile_counting = 0;
                uint32_t dump_addr = prim_vtx_addr(vbase,ibase,idxfmt,i-1,vf.stride);
                s_primitive_profile_counting = s_primitive_profile;
                near_dump("line", dump_addr, &vf, both, 2);
                continue;
            }
            if (da < 0.0f)       { CVtx t; clip_lerp_cvtx(&ca,&cb, da/(da-db2), &t); ca=t; }
            else if (db2 < 0.0f) { CVtx t; clip_lerp_cvtx(&cb,&ca, db2/(db2-da), &t); cb=t; }
            if (ca.cw <= NEAR_W || cb.cw <= NEAR_W) { s_stat.nearclip++; continue; }
            /* PPSSPP ProcessLine: clip-created endpoints outside the screen grid kill the line. */
            if (clip_vtx_out_of_range(&ca) || clip_vtx_out_of_range(&cb)) { s_stat.nearclip++; continue; }
            Vtx a, b;
            project_cvtx(&ca,&a); project_cvtx(&cb,&b);
            draw_line_vtx(&a,&b,1);
        }
    } else if (type == 3 || type == 4 || type == 5) {
        /* SR_RTRACE per-draw bookkeeping: per-reason triangle counters plus a snapshot of the
         * pixel-level stat counters so the TRIDRW+ line can report this draw's pixel outcome. */
        enum { D_DRAW, D_OOR, D_WNEG, D_ZOUT, D_ZSAME, D_NEARALL, D_CWLOW, D_CLIP };
        static const char *dec_name[8] =
            { "DRAW", "drop-oor", "drop-wneg", "drop-zout", "drop-zsame", "drop-nearall", "drop-cwlow", "CLIP" };
        unsigned long dcnt[8] = {0,0,0,0,0,0,0,0};
        struct { unsigned long tri3d, cull, zfail, afail, px3d, zrange, deg3d, blk3d; } st0;
        st0.tri3d = s_stat.tri3d; st0.cull = s_stat.cull; st0.zfail = s_stat.zfail;
        st0.afail = s_stat.afail; st0.px3d = s_stat.px3d; st0.zrange = s_stat.zrange;
        st0.deg3d = s_stat.deg3d; st0.blk3d = s_stat.blk3d;
        int tlogged = 0;
        float roffx = (float)(ge.offsetx & 0xFFFF) / 16.0f;
        float roffy = (float)(ge.offsety & 0xFFFF) / 16.0f;
        if (rt) {
            fprintf(stderr, "TRIDRW f=%u d=%u fbp=0x%08x/%u zbp=0x%08x/%u type=%d n=%d vtype=0x%06x idx=%d va=0x%08x ia=0x%08x "
                    "tex=%d/0x%08x %ux%u fmt=%u bufw=%u sw=%u tfunc=0x%06x tmm=0x%03x lit=%d gou=%d "
                    "zt=%d/%d zw=%d zr=[%u,%u] at=%d/0x%06x bl=%d/%06x/%06x/%06x cull=%d/%u clr=0x%03x "
                    "dclip=%u fog=%d vp=[%.4g,%.4g,%.4g|%.4g,%.4g,%.4g] off=(%.4g,%.4g) sc=(%d,%d)-(%d,%d) "
                    "wt=(%.4g,%.4g,%.4g) vt=(%.4g,%.4g,%.4g)\n",
                    s_ge_frame, s_rt_draw, ge.fbp, ge.fbw, ge.zbp, ge.zbw, type, count, ge.vtype, idxfmt, vbase, ibase,
                    ge.tex_enable, ge.tex_addr, ge.tex_w, ge.tex_h, ge.tex_fmt, ge.tex_bufw,
                    ge.tex_swizzle, ge.tex_func, ge.tex_map_mode, ge.lighting_enable, ge.shade_gouraud,
                    ge.ztest_enable, (int)(ge.ztest & 7), !ge.zwrite_disable, ge.minz, ge.maxz,
                    ge.atest_enable, ge.atest, ge.blend_enable, ge.blend_mode, ge.blend_fixa, ge.blend_fixb,
                    ge.cull_enable, ge.cull, ge.clear_mode, ge.depth_clip, ge.fog_enable,
                    decode_float24(ge.viewport_raw[0]), decode_float24(ge.viewport_raw[1]),
                    decode_float24(ge.viewport_raw[2]), decode_float24(ge.viewport_raw[3]),
                    decode_float24(ge.viewport_raw[4]), decode_float24(ge.viewport_raw[5]),
                    roffx, roffy, ge.scis_x1, ge.scis_y1, ge.scis_x2, ge.scis_y2,
                    ge.world[9], ge.world[10], ge.world[11], ge.view[9], ge.view[10], ge.view[11]);
        }
        int cache_strip = type == 4 && strip_cache_enabled() && count >= 3;
        CVtx strip_prev[2];
        if (cache_strip) {
            transform_vtx_clip(prim_vtx_addr(vbase,ibase,idxfmt,0,vf.stride),&vf,&strip_prev[0]);
            transform_vtx_clip(prim_vtx_addr(vbase,ibase,idxfmt,1,vf.stride),&vf,&strip_prev[1]);
        }
        for (int i = 2; i < count; i++) {
            int i0, i1, i2;
            if      (type == 3) { if (i%3!=2) continue; i0=i-2; i1=i-1; i2=i; }
            else if (type == 4) { i0=i-2; i1=i-1; i2=i; }
            else                { i0=0;   i1=i-1; i2=i; }
            int tri_profile_slot = primitive_profile_claim_triangle();
            CVtx cv[3], cl[4];
            if (cache_strip) {
                cv[0] = strip_prev[0]; cv[1] = strip_prev[1];
                transform_vtx_clip(prim_vtx_addr(vbase,ibase,idxfmt,i2,vf.stride),&vf,&cv[2]);
                strip_prev[0] = strip_prev[1]; strip_prev[1] = cv[2];
            } else {
                transform_vtx_clip(prim_vtx_addr(vbase,ibase,idxfmt,i0,vf.stride),&vf,&cv[0]);
                transform_vtx_clip(prim_vtx_addr(vbase,ibase,idxfmt,i1,vf.stride),&vf,&cv[1]);
                transform_vtx_clip(prim_vtx_addr(vbase,ibase,idxfmt,i2,vf.stride),&vf,&cv[2]);
            }

            uint64_t acceptance_started = tri_profile_slot == 0 ? SDL_GetTicksNS() : 0;

            /* Culling via submission order (PPSSPP TransformUnit::SendTriangle): the
             * rasterizer keeps only positive-winding triangles, so cull mode 0 (CW front)
             * reverses the order, and STRIPS flip it again on every other triangle because
             * strip triangles alternate winding (cullType ^ wind). Without the parity flip
             * every second strip triangle is culled wrongly and back faces leak through —
             * meshes dissolve into scattered triangles. The last vertex (provoking, used
             * for flat shading) stays in place. */
            if (ge.cull_enable && !ge.clear) {
                int flip = ((ge.cull & 1) == 0) ^ ((type == 4) ? (i & 1) : 0);
                if (flip) { CVtx t = cv[0]; cv[0] = cv[1]; cv[1] = t; }
            }

            /* PSP triangle acceptance (PPSSPP Clipper::ProcessTriangle). The hardware DROPS
             * whole triangles rather than clipping when vertices leave the screen grid or the
             * z range; games rely on it (effect passes, behind-camera geometry). Drawing them
             * (the old behaviour) scattered runaway triangles across the scene. */
            int dec = D_DRAW;
            if (cv[0].oor || cv[1].oor || cv[2].oor) dec = D_OOR;
            else if (cv[0].cw < 0.0f && cv[1].cw < 0.0f && cv[2].cw < 0.0f) dec = D_WNEG;
            else {
                int zp = cv[0].ozp + cv[1].ozp + cv[2].ozp;
                int zn = cv[0].ozn + cv[1].ozn + cv[2].ozn;
                if (!ge.depth_clip && zp + zn > 0) dec = D_ZOUT;
                else if (zp >= 3 || zn >= 3) dec = D_ZSAME;
            }
            if (dec == D_DRAW) {
                if (clip_d_nearz(&cv[0]) >= 0.0f && clip_d_nearz(&cv[1]) >= 0.0f && clip_d_nearz(&cv[2]) >= 0.0f) {
                    if (!(cv[0].cw > NEAR_W && cv[1].cw > NEAR_W && cv[2].cw > NEAR_W)) dec = D_CWLOW;
                } else if (clip_d_nearz(&cv[0]) < 0.0f && clip_d_nearz(&cv[1]) < 0.0f && clip_d_nearz(&cv[2]) < 0.0f) {
                    dec = D_NEARALL;
                } else {
                    dec = D_CLIP;   /* crosses the z >= -w plane: clip and fan */
                }
            }
            dcnt[dec]++;
            if (s_primitive_profile_counting) {
                if (dec == D_DRAW) s_cpu_profile_stats.primitive_profile_transform_triangles_drawn++;
                else if (dec == D_CLIP) s_cpu_profile_stats.primitive_profile_transform_triangles_clipped++;
                else s_cpu_profile_stats.primitive_profile_transform_triangles_rejected++;
            }
            if (dec != D_DRAW) s_stat.nearclip++;
            int clip_m = -1;
            if (dec == D_CLIP)
                clip_m = clip_poly_near(cv, 3, cl);
            if (acceptance_started)
                primitive_profile_end(GE_PRIM_PROFILE_CLIPPING_ACCEPTANCE, acceptance_started);

            /* TRIDEC: per-triangle decision log. First 12 per draw, plus EVERY triangle whose
             * screen bbox covers the SR_PIXWHO probe pixel (drawn or dropped) — the probe
             * coverage test treats any cw<=0 vertex as unbounded (could project anywhere). */
            if (rt) {
                int probe = 0;
                if (s_who_x >= 0) {
                    float bx0 = 1e30f, bx1 = -1e30f, by0 = 1e30f, by1 = -1e30f;
                    int unbounded = 0;
                    for (int k = 0; k < 3; k++) {
                        if (cv[k].cw <= 0.0f) { unbounded = 1; continue; }
                        float px = cv[k].sx - roffx, py = cv[k].sy - roffy;
                        if (px < bx0) bx0 = px;
                        if (px > bx1) bx1 = px;
                        if (py < by0) by0 = py;
                        if (py > by1) by1 = py;
                    }
                    if (unbounded) probe = 1;
                    else probe = (bx0 <= (float)s_who_x + 1 && bx1 >= (float)s_who_x - 1 &&
                                  by0 <= (float)s_who_y + 1 && by1 >= (float)s_who_y - 1);
                }
                if (probe || tlogged < 12) {
                    tlogged++;
                    fprintf(stderr, "TRIDEC f=%u d=%u i=%d %s%s"
                            " v0[c=(%.4g,%.4g,%.4g,%.4g) s=(%.1f,%.1f,%.0f) f=%d%d%d rgba=%d,%d,%d,%d uv=(%.3g,%.3g)]"
                            " v1[c=(%.4g,%.4g,%.4g,%.4g) s=(%.1f,%.1f,%.0f) f=%d%d%d rgba=%d,%d,%d,%d uv=(%.3g,%.3g)]"
                            " v2[c=(%.4g,%.4g,%.4g,%.4g) s=(%.1f,%.1f,%.0f) f=%d%d%d rgba=%d,%d,%d,%d uv=(%.3g,%.3g)]\n",
                            s_ge_frame, s_rt_draw, i, dec_name[dec], probe ? " PROBE" : "",
                            cv[0].cx, cv[0].cy, cv[0].cz, cv[0].cw, cv[0].sx - roffx, cv[0].sy - roffy, cv[0].sz,
                            cv[0].oor, cv[0].ozp, cv[0].ozn, cv[0].r, cv[0].g, cv[0].b, cv[0].a, cv[0].u, cv[0].v,
                            cv[1].cx, cv[1].cy, cv[1].cz, cv[1].cw, cv[1].sx - roffx, cv[1].sy - roffy, cv[1].sz,
                            cv[1].oor, cv[1].ozp, cv[1].ozn, cv[1].r, cv[1].g, cv[1].b, cv[1].a, cv[1].u, cv[1].v,
                            cv[2].cx, cv[2].cy, cv[2].cz, cv[2].cw, cv[2].sx - roffx, cv[2].sy - roffy, cv[2].sz,
                            cv[2].oor, cv[2].ozp, cv[2].ozn, cv[2].r, cv[2].g, cv[2].b, cv[2].a, cv[2].u, cv[2].v);
                }
            }

            if (dec == D_DRAW) {
                Vtx a, b, c;
                uint64_t assembly_started = tri_profile_slot == 1 ? SDL_GetTicksNS() : 0;
                project_cvtx(&cv[0],&a); project_cvtx(&cv[1],&b); project_cvtx(&cv[2],&c);
                if (assembly_started)
                    primitive_profile_end(GE_PRIM_PROFILE_ASSEMBLY, assembly_started);
                primitive_profile_finish_total();
                raster_tri(&a,&b,&c,1);
            } else if (dec == D_CLIP) {
                int m = clip_m;
                if (m >= 3) {
                    /* PPSSPP discards each SUB-triangle whose vertices include a clip-created
                     * vertex that re-projects outside the screen grid (clip_vtx_out_of_range);
                     * the rest of the fan still draws. */
                    Vtx p[4];
                    int bad[4];
                    uint64_t assembly_started = tri_profile_slot == 1 ? SDL_GetTicksNS() : 0;
                    for (int k = 0; k < m && k < 4; k++) {
                        bad[k] = (cl[k].cw <= NEAR_W) || clip_vtx_out_of_range(&cl[k]);
                        if (!bad[k]) project_cvtx(&cl[k], &p[k]);
                    }
                    if (assembly_started)
                        primitive_profile_end(GE_PRIM_PROFILE_ASSEMBLY, assembly_started);
                    primitive_profile_finish_total();
                    if (rt && tlogged <= 13) {
                        char buf[640]; int bn = 0;
                        for (int k = 0; k < m && k < 4; k++)
                            bn += snprintf(buf + bn, sizeof(buf) - (size_t)bn,
                                           " p%d[c=(%.4g,%.4g,%.4g,%.4g)->(%.1f,%.1f,%.0f) bad=%d rgba=%d,%d,%d,%d]",
                                           k, cl[k].cx, cl[k].cy, cl[k].cz, cl[k].cw,
                                           bad[k] ? -1.0f : p[k].x, bad[k] ? -1.0f : p[k].y,
                                           bad[k] ? -1.0f : p[k].z, bad[k],
                                           cl[k].r, cl[k].g, cl[k].b, cl[k].a);
                        fprintf(stderr, "TRICLP f=%u d=%u i=%d m=%d%s\n",
                                s_ge_frame, s_rt_draw, i, m, buf);
                    }
                    for (int k = 2; k < m && k < 4; k++)
                        if (!bad[0] && !bad[k-1] && !bad[k])
                            raster_tri(&p[0], &p[k-1], &p[k], 1);
                }
                if (m < 3)
                    primitive_profile_finish_total();
            } else {
                primitive_profile_finish_total();
            }
        }
        if (rt) {
            fprintf(stderr, "TRIDRW+ f=%u d=%u tris=%lu draw=%lu clip=%lu drop[oor=%lu wneg=%lu zout=%lu "
                    "zsame=%lu nearall=%lu cwlow=%lu] rast=%lu cull=%lu deg=%lu px=%lu zfail=%lu afail=%lu "
                    "zrange=%lu blk=%lu\n",
                    s_ge_frame, s_rt_draw,
                    dcnt[0]+dcnt[1]+dcnt[2]+dcnt[3]+dcnt[4]+dcnt[5]+dcnt[6]+dcnt[7],
                    dcnt[D_DRAW], dcnt[D_CLIP], dcnt[D_OOR], dcnt[D_WNEG], dcnt[D_ZOUT],
                    dcnt[D_ZSAME], dcnt[D_NEARALL], dcnt[D_CWLOW],
                    s_stat.tri3d - st0.tri3d, s_stat.cull - st0.cull, s_stat.deg3d - st0.deg3d,
                    s_stat.px3d - st0.px3d, s_stat.zfail - st0.zfail, s_stat.afail - st0.afail,
                    s_stat.zrange - st0.zrange, s_stat.blk3d - st0.blk3d);
        }
    } else if (type == 6) {
        /* Sprites in transform mode: two 3D corner vertices define an axis-aligned screen rect.
         * The Z/depth comes from the second vertex. */
        for (int i = 1; i < count; i += 2) {
            Vtx p0, p1;
            if (!transform_vtx_3d(prim_vtx_addr(vbase,ibase,idxfmt,i-1,vf.stride),&vf,&p0)) continue;
            if (!transform_vtx_3d(prim_vtx_addr(vbase,ibase,idxfmt,i,  vf.stride),&vf,&p1)) continue;
            fill_sprite(&p0,&p1,1);
        }
    }
    s_primitive_profile_counting = 0;
    primitive_profile_clear_pending();
}

/* Unconditional framebuffer snapshot (no "best frame" gating) for SR_FBSNAP. */
static void ge_snapshot_plain(const char *path) {
    uint32_t fb = ge_fb_addr(), stride = ge.fbw ? ge.fbw : 512;
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n480 272\n255\n");
    for (int y = 0; y < 272; y++)
        for (int x = 0; x < 480; x++) {
            int r, g, b, a;
            if ((ge.fbfmt & 3) == 3) unpack_color(MEM_R32(fb + (uint32_t)(y*(int)stride + x)*4), 3, &r, &g, &b, &a);
            else                     unpack_color(MEM_R16(fb + (uint32_t)(y*(int)stride + x)*2), ge.fbfmt & 3, &r, &g, &b, &a);
            unsigned char px[3] = { (unsigned char)r, (unsigned char)g, (unsigned char)b };
            fwrite(px, 1, 3, f);
        }
    fclose(f);
}

/* Snapshot the current GE target framebuffer to a PPM. */
static void ge_snapshot(const char *path) {
    uint32_t fb=ge_fb_addr(), stride=ge.fbw?ge.fbw:512;
    unsigned char img[272][480][3]; long nb=0;
    for (int y=0; y<272; y++) for (int x=0; x<480; x++) {
        int r,g,b,a;
        if ((ge.fbfmt&3)==3) unpack_color(MEM_R32(fb+(uint32_t)(y*(int)stride+x)*4),3,&r,&g,&b,&a);
        else                 unpack_color(MEM_R16(fb+(uint32_t)(y*(int)stride+x)*2),ge.fbfmt&3,&r,&g,&b,&a);
        img[y][x][0]=(unsigned char)r; img[y][x][1]=(unsigned char)g; img[y][x][2]=(unsigned char)b;
        if (r|g|b) nb++;
    }
    static long best=-1; if (nb<=best) return; best=nb;
    FILE *f=fopen(path,"wb"); if(!f) return;
    fprintf(f,"P6\n480 272\n255\n"); fwrite(img,1,sizeof(img),f); fclose(f);
}

static const char *ge_cmd_name(uint32_t cmd) {
    switch (cmd) {
        case 0x05: return "BEZIER";           case 0x06: return "SPLINE";
        case 0x07: return "BOUNDINGBOX";
        case 0x2A: return "BONEMATRIXNUMBER"; case 0x2B: return "BONEMATRIXDATA";
        case 0x40: return "TGENMATRIXNUMBER"; case 0x41: return "TGENMATRIXDATA";
        case 0xB2: return "TRANSFERSRC";      case 0xB3: return "TRANSFERSRCW";
        case 0xB4: return "TRANSFERDST";      case 0xB5: return "TRANSFERDSTW";
        case 0xD8: return "COLORTEST";        case 0xD9: return "COLORREF";
        case 0xDA: return "COLORTESTMASK";    case 0xE6: return "LOGICOP";
        case 0xEA: return "TRANSFERSTART";    case 0xEB: return "TRANSFERSRCPOS";
        case 0xEC: return "TRANSFERDSTPOS";   case 0xEE: return "TRANSFERSIZE";
        default: return NULL;
    }
}

unsigned long g_ge_unhandled_mask[8];
static void ge_note_unhandled_cmd(uint32_t cmd, uint32_t op) {
    static int on=-1;
    if (on<0) { const char *e=getenv("SR_GEUNK"); on=(e&&e[0]=='0')?0:1; }
    cmd &= 0xFF;
    unsigned long bit = 1ul << (cmd & 31);
    if (g_ge_unhandled_mask[cmd>>5] & bit) return;
    g_ge_unhandled_mask[cmd>>5] |= bit;
    if (on) {
        const char *nm=ge_cmd_name(cmd);
        fprintf(stderr,"GE: unhandled command 0x%02x (%s) data=0x%06x [reported once]\n",
                cmd, nm?nm:"unknown", op&0xFFFFFF);
    }
}

unsigned long g_ge_list_sig=0, g_ge_prim_count=0;
/* Per-list write accounting for SR_GEWATCH: shows whether a list that cleared a buffer
 * also drew non-black content into it, and which buffer it targeted. */
unsigned long g_list_writes=0, g_list_nonblack=0, g_list_clearpx=0;
static uint32_t ge_run_list_inner(uint32_t addr, int resume);

uint32_t ge_run_list(uint32_t addr, int resume) {
    ge_cpu_profile_configure();
    uint64_t nested_before = 0;
    if (s_cpu_profile)
        nested_before = s_cpu_profile_stats.phase[GE_CPU_PRIMITIVE].ns +
                        s_cpu_profile_stats.phase[GE_CPU_BLOCK_TRANSFER].ns +
                        s_cpu_profile_stats.phase[GE_CPU_CLUT_LOAD].ns +
                        s_cpu_profile_stats.phase[GE_CPU_FLUSH].ns;
    uint64_t profile_started = ge_cpu_profile_begin();
    unsigned long t0 = wall_ms();
    uint32_t next_addr = ge_run_list_inner(addr, resume);
    s_ge_ms_acc += wall_ms() - t0;
    if (s_cpu_profile) {
        uint64_t total = SDL_GetTicksNS() - profile_started;
        uint64_t nested_after = 0;
        nested_after = s_cpu_profile_stats.phase[GE_CPU_PRIMITIVE].ns +
                       s_cpu_profile_stats.phase[GE_CPU_BLOCK_TRANSFER].ns +
                       s_cpu_profile_stats.phase[GE_CPU_CLUT_LOAD].ns +
                       s_cpu_profile_stats.phase[GE_CPU_FLUSH].ns;
        uint64_t nested = nested_after - nested_before;
        s_cpu_profile_stats.phase[GE_CPU_LIST_TOTAL].calls++;
        s_cpu_profile_stats.phase[GE_CPU_LIST_TOTAL].ns += total;
        s_cpu_profile_stats.phase[GE_CPU_COMMAND_DISPATCH].calls++;
        s_cpu_profile_stats.phase[GE_CPU_COMMAND_DISPATCH].ns += total > nested ? total - nested : 0;
    }
    return next_addr;
}

/* GE block transfer (the "memcpy engine"): rectangle copy between guest buffers, used for
 * framebuffer effects (pause capture, blur, scene transitions). Decode masks follow PPSSPP
 * GPUState.h. With a GPU backend the source may live in a GPU image — flush("xfersrc")
 * reads overlapping targets back to guest VRAM first — and the destination may be
 * GPU-cached, so the written range is dirty-notified afterwards. */
static void ge_block_transfer(uint32_t startdata) {
    uint32_t srcBase = (ge.xf_src & 0xFFFFF0u) | ((ge.xf_srcw & 0xFF0000u) << 8);
    uint32_t dstBase = (ge.xf_dst & 0xFFFFF0u) | ((ge.xf_dstw & 0xFF0000u) << 8);
    uint32_t srcStride = ge.xf_srcw & 0x7F8u; if (srcStride > 0x400) srcStride = 0;
    uint32_t dstStride = ge.xf_dstw & 0x7F8u; if (dstStride > 0x400) dstStride = 0;
    uint32_t srcX = ge.xf_spos & 0x3FFu, srcY = (ge.xf_spos >> 10) & 0x3FFu;
    uint32_t dstX = ge.xf_dpos & 0x3FFu, dstY = (ge.xf_dpos >> 10) & 0x3FFu;
    uint32_t w = (ge.xf_size & 0x3FFu) + 1, h = ((ge.xf_size >> 10) & 0x3FFu) + 1;
    uint32_t bpp = (startdata & 1) ? 4 : 2;
    if (!srcStride || !dstStride) return;
    if (s_gpu && s_gpu->xfer && s_gpu->xfer(startdata)) return;   /* GPU-side blit */
    if (s_gpu && s_gpu->flush) s_gpu->flush("xfersrc");
    { uint32_t so0 = srcBase + (srcY * srcStride + srcX) * bpp;
      uint32_t do0 = dstBase + (dstY * dstStride + dstX) * bpp;
      if (!sr_inrange(so0) || !sr_inrange(do0)) {
          fprintf(stderr, "ge_block_transfer: OOR src=0x%08x dst=0x%08x, skipping CPU blit\n", so0, do0);
          goto skip_cpu_blit;
      }
    }
    for (uint32_t y = 0; y < h; y++) {
        uint32_t so = srcBase + ((srcY + y) * srcStride + srcX) * bpp;
        uint32_t dofs = dstBase + ((dstY + y) * dstStride + dstX) * bpp;
        ge_capture_note_memory(so, w * bpp);
        ge_capture_note_memory(dofs, w * bpp);
        if (g_sr_heap_watch) sr_heap_note_bulk_write(dofs, w * bpp, 0u);
        memmove(SR_HOST(dofs), SR_HOST(so), (size_t)w * bpp);
        sr_gpu_vram_dirty(dofs, w * bpp);
    }
  skip_cpu_blit: ;
}

static uint32_t ge_run_list_inner(uint32_t addr, int resume) {
    if (!s_ge_inited) ge_state_init();
    if (!resume) {
        s_sp = 0;
    }
    static int snap=-1; if(snap<0) snap=(getenv("SR_FBDUMP")||getenv("SR_GEDUMP"))?1:0;
    uint32_t list_addr=addr;
    ge_capture_configure();
    if (!s_capture_done && !ge_capture_active() && s_ge_frame >= s_capture_frame &&
        s_ge_frame <= s_capture_frame_end) {
        int boundary_ok = !s_gpu || !s_gpu->capture_boundary || s_gpu->capture_boundary();
        if (!boundary_ok) {
            fprintf(stderr, "GE_CAPTURE: GPU boundary materialization failed\n");
        } else if (ge_capture_begin(s_capture_path, s_ge_frame, list_addr, &ge, s_zbuf)) {
            s_capture_total_prims = 0;
        }
    }
    /* begin() records list_addr even when this invocation resumes a stalled list.
     * Later fresh lists in the same frame are appended here. */
    else if (ge_capture_active() && !resume && !ge_capture_add_list(list_addr)) {
        fprintf(stderr, "GE_CAPTURE: could not append list 0x%08x\n", list_addr);
        ge_capture_abort();
    }
    g_list_writes=0; g_list_nonblack=0; g_list_clearpx=0;
    g_ge_list_through_cmds=0; g_ge_list_transform_cmds=0;
    g_ge_list_through_vertices=0; g_ge_list_transform_vertices=0;
    g_ge_list_through_sprites=0; g_ge_list_transform_sprites=0;
    extern unsigned long g_tex_nonzero; unsigned long start_nz=g_tex_nonzero;
    unsigned long sig=0; unsigned long prims=0;
    int pending_signal=0;
    uint32_t pending_signal_op=0;
    for (int guard=0; guard<(1<<20); guard++) {
        if (g_ge_stall_addr && addr == g_ge_stall_addr) {
            static int s_gelog_stall = -1;
            if (s_gelog_stall < 0) s_gelog_stall = getenv("SR_GELOG") ? 1 : 0;
            if (s_gelog_stall) {
                fprintf(stderr, "GE STALL reached: 0x%08x\n", addr);
            }
            return addr;
        }
        uint32_t op=MEM_R32(addr); addr+=4;
        uint32_t cmd=op>>24, data=op&0xFFFFFF;
        if (s_cpu_profile) s_cpu_profile_stats.commands++;
        sig=sig*1000003ul+op;
        switch (cmd) {
            case GE_NOP: case 0xFF: break;   /* 0xFF = GE_CMD_NOP_FF (padding, PPSSPP) */
            case GE_VADDR:    ge.vaddr=ge_rel_addr(data); break;
            case GE_IADDR:    ge.iaddr=ge_rel_addr(data); break;
            case GE_BASE:     ge.base=data; break;
            case GE_OFFSETADDR: ge.offset=data<<8; break;
            case GE_ORIGIN:   ge.offset=addr-4; break;
            case GE_VERTEXTYPE: ge.vtype=data; break;

            case GE_FRAMEBUFPTR:   ge.fbp=ge_normalize_vram_target((ge.fbp&0xFF000000)|data);
                if (s_gelog>0) fprintf(stderr,"GE FRAMEBUFPTR data=0x%06x -> fbp=0x%08x\n",data,ge.fbp);
                break;
            case GE_FRAMEBUFWIDTH: ge.fbw=data&0xFFFF; ge.fbp=ge_normalize_vram_target((ge.fbp&0x00FFFFFF)|((data&0xFF0000)<<8));
                if (s_gelog>0) fprintf(stderr,"GE FRAMEBUFWIDTH data=0x%06x -> fbw=%u fbp=0x%08x\n",data,ge.fbw,ge.fbp);
                break;
            case GE_FRAMEBUFPIXFORMAT: ge.fbfmt=data&3; break;

            case GE_ZBUFPTR:   ge.zbp=ge_normalize_vram_target((ge.zbp&0xFF000000)|data); break;
            case GE_ZBUFWIDTH: ge.zbw=data&0xFFFF; ge.zbp=ge_normalize_vram_target((ge.zbp&0x00FFFFFF)|((data&0xFF0000)<<8)); break;

            /* Drawing region: the scissor (which the game also sets) bounds all writes. */
            case GE_REGION1: case GE_REGION2: break;

            case GE_SCISSOR1: ge.scis_x1=data&0x3FF; ge.scis_y1=(data>>10)&0x3FF; break;
            case GE_SCISSOR2: ge.scis_x2=data&0x3FF; ge.scis_y2=(data>>10)&0x3FF; break;

            /* ---- Depth / cull state ---- */
            case GE_ZTESTENABLE:   ge.ztest_enable  = data & 1; break;
            case GE_ZWRITEDISABLE: ge.zwrite_disable= data & 1; break;
            case GE_ZTEST:         ge.ztest         = data & 7; break;
            case GE_MINZ:          ge.minz          = data & 0xFFFF; break;
            case GE_MAXZ:          ge.maxz          = data & 0xFFFF; break;
            case GE_CULLFACEENABLE:ge.cull_enable   = data & 1; break;
            case GE_CULL:          ge.cull          = data & 1; break;
            case GE_CLIPENABLE:    ge.depth_clip = data & 1; break;

            /* ---- Fragment test / blend toggles ---- */
            case GE_ALPHATESTENABLE: ge.atest_enable = data & 1; break;
            case GE_ALPHATEST:       ge.atest        = data; break;
            case GE_ALPHABLENDENABLE: ge.blend_enable=data&1; break;
            case GE_FOGENABLE:  ge.fog_enable = data & 1; break;
            case GE_FOG1:       ge.fog_end   = decode_float24(data); break;
            case GE_FOG2:       ge.fog_slope = decode_float24(data); break;
            case GE_FOGCOLOR:   ge.fog_color = data; break;
            case GE_DITHERENABLE: ge.dither_enable = data & 1; break;
            case GE_DITH0: case GE_DITH1: case GE_DITH2: case GE_DITH3: {
                /* One row of the 4x4 dither matrix: 4 signed 4-bit values. */
                int row = (int)(cmd - GE_DITH0);
                for (int i = 0; i < 4; i++) {
                    int v = (int)((data >> (i*4)) & 0xF);
                    ge.dith[row][i] = (int8_t)(v >= 8 ? v - 16 : v);
                }
                break;
            }
            case GE_MASKRGB:   ge.mask_rgb   = data; break;
            case GE_MASKALPHA: ge.mask_alpha = (int)(data & 0xFF); break;
            case GE_STENCILTESTENABLE: ge.stencil_enable=data&1; break;
            case GE_STENCILTEST:       ge.stencil_test=data; break;
            case GE_STENCILOP:         ge.stencil_op=data; break;
            case GE_COLORTESTENABLE:   ge.color_test_enable=data&1; break;
            case GE_COLORTEST:         ge.color_test=data&3; break;
            case GE_COLORREF:          ge.color_ref=data&0x00FFFFFFu; break;
            case GE_COLORTESTMASK:     ge.color_test_mask=data&0x00FFFFFFu; break;
            case GE_LOGICOPENABLE:     ge.logic_op_enable=data&1; break;
            case GE_LOGICOP:           ge.logic_op=data&15; break;
            case GE_ANTIALIASENABLE: break;
            case GE_PATCHCULLENABLE: ge.patch_cull_enable=data&1; break;
            case GE_PATCHDIVISION:   ge.patch_division=data; break;
            case GE_PATCHPRIMITIVE:  ge.patch_primitive=data&3; break;
            case GE_PATCHFACING:     ge.patch_facing=data&1; break;

            /* ---- Matrix capture ----
             * *MATRIXNUMBER sets the write cursor; *MATRIXDATA writes one float24 and advances.
             * World/View are 4×3 (12 elements), Proj is 4×4 (16 elements). */
            /* Bone matrices: 8 4x3 matrices in one 96-float array; NUMBER takes data&0x7F
             * (a raw cursor, can point past the end), DATA writes land only while < 96
             * (PPSSPP SoftGpu.cpp). The hangar aircraft is drawn part-by-part with one
             * rigid bone per part and single-weight vertices. */
            case 0x2A: ge.bone_num = (int)(data & 0x7F); break;    /* BONEMATRIXNUMBER */
            case 0x2B:                                             /* BONEMATRIXDATA */
                if (s_stat_on > 0) s_stat.bonew++;
                if (ge.bone_num < 96) ge.bone[ge.bone_num] = decode_float24(data);
                ge.bone_num++;
                break;
            case GE_MORPHWEIGHT0+0: case GE_MORPHWEIGHT0+1:
            case GE_MORPHWEIGHT0+2: case GE_MORPHWEIGHT0+3:
            case GE_MORPHWEIGHT0+4: case GE_MORPHWEIGHT0+5:
            case GE_MORPHWEIGHT0+6: case GE_MORPHWEIGHT0+7:
                ge.morph_weight[cmd-GE_MORPHWEIGHT0]=decode_float24(data);
                break;
            /* Hardware semantics (PPSSPP SoftGpu.cpp): NUMBER takes the low 4 bits; DATA writes
             * advance the cursor unconditionally but only land while it is in range -- excess
             * words are DISCARDED, never wrapped. (The old wrap-to-0 let a 13th data word clobber
             * element 0: the hangar/briefing engine path uploads 13-word matrices and every one
             * of its world matrices was corrupted, throwing whole meshes behind the camera.) */
            case GE_WORLDMATRIXNUMBER: ge.world_num = (int)(data & 0xF); break;
            case GE_WORLDMATRIXDATA:
                s_stat.mw_world++;
                if (ge.world_num < 12) ge.world[ge.world_num] = decode_float24(data);
                ge.world_num++;
                break;
            case GE_VIEWMATRIXNUMBER: ge.view_num = (int)(data & 0xF); break;
            case GE_VIEWMATRIXDATA:
                s_stat.mw_view++;
                trace_mtx_write("view", ge.view_num, data);
                if (ge.view_num < 12) ge.view[ge.view_num] = decode_float24(data);
                ge.view_num++;
                break;
            case GE_PROJMATRIXNUMBER: ge.proj_num = (int)(data & 0xF); break;
            case GE_PROJMATRIXDATA:
                s_stat.mw_proj++;
                trace_mtx_write("proj", ge.proj_num, data);
                if (ge.proj_num < 16) ge.proj[ge.proj_num] = decode_float24(data);
                ge.proj_num++;
                break;

            /* ---- Viewport / offset ---- */
            case GE_VIEWPORTXSCALE:  ge.viewport_raw[0]=data; break;
            case GE_VIEWPORTYSCALE:  ge.viewport_raw[1]=data; break;
            case GE_VIEWPORTZSCALE:  ge.viewport_raw[2]=data; break;
            case GE_VIEWPORTXCENTER: ge.viewport_raw[3]=data; break;
            case GE_VIEWPORTYCENTER: ge.viewport_raw[4]=data; break;
            case GE_VIEWPORTZCENTER: ge.viewport_raw[5]=data; break;
            case GE_OFFSETX:         ge.offsetx=data; break;
            case GE_OFFSETY:         ge.offsety=data; break;

            /* ---- Shading / material / lighting ---- */
            case GE_SHADEMODE:       ge.shade_gouraud = data & 1; break;
            case GE_REVERSENORMAL:   ge.reverse_normal = data & 1; break;
            case GE_MATERIALUPDATE:  ge.mat_update = data & 7; break;
            case GE_MATERIALEMISSIVE:ge.mat_emissive = data; break;
            case GE_MATERIALAMBIENT: ge.material=data; ge.material_set=1; break;
            case GE_MATERIALDIFFUSE: ge.mat_diffuse=data;
                /* Flat-colour fallback for colourless vertices also tracks diffuse. */
                ge.material=data; ge.material_set=1; break;
            case GE_MATERIALSPECULAR:ge.mat_specular = data; break;
            case GE_MATERIALALPHA:   ge.material_alpha=data&0xFF; ge.material_alpha_set=1; break;
            case GE_MATERIALSPECULARCOEF: ge.spec_coef = decode_float24(data); break;
            case GE_AMBIENTCOLOR:    ge.amb_color = data; break;
            case GE_AMBIENTALPHA:    ge.amb_alpha = (int)(data & 0xFF); break;
            case GE_LIGHTMODE:       ge.light_mode = data & 1; break;
            case GE_LIGHTINGENABLE:  ge.lighting_enable = data & 1; break;
            case GE_LIGHTENABLE0+0: case GE_LIGHTENABLE0+1:
            case GE_LIGHTENABLE0+2: case GE_LIGHTENABLE0+3:
                ge.light_on[cmd-GE_LIGHTENABLE0] = data & 1; break;
            case GE_LIGHTTYPE0+0: case GE_LIGHTTYPE0+1:
            case GE_LIGHTTYPE0+2: case GE_LIGHTTYPE0+3:
                ge.light_type[cmd-GE_LIGHTTYPE0] = data; break;
            case GE_LX0+0:  case GE_LX0+1:  case GE_LX0+2:  case GE_LX0+3:
            case GE_LX0+4:  case GE_LX0+5:  case GE_LX0+6:  case GE_LX0+7:
            case GE_LX0+8:  case GE_LX0+9:  case GE_LX0+10: case GE_LX0+11: {
                int k=(int)(cmd-GE_LX0); ge.light_pos[k/3][k%3]=decode_float24(data); break;
            }
            case GE_LDX0+0: case GE_LDX0+1: case GE_LDX0+2: case GE_LDX0+3:
            case GE_LDX0+4: case GE_LDX0+5: case GE_LDX0+6: case GE_LDX0+7:
            case GE_LDX0+8: case GE_LDX0+9: case GE_LDX0+10:case GE_LDX0+11: {
                int k=(int)(cmd-GE_LDX0); ge.light_dir[k/3][k%3]=decode_float24(data); break;
            }
            case GE_LKA0+0: case GE_LKA0+1: case GE_LKA0+2: case GE_LKA0+3:
            case GE_LKA0+4: case GE_LKA0+5: case GE_LKA0+6: case GE_LKA0+7:
            case GE_LKA0+8: case GE_LKA0+9: case GE_LKA0+10:case GE_LKA0+11: {
                int k=(int)(cmd-GE_LKA0); ge.light_att[k/3][k%3]=decode_float24(data); break;
            }
            case GE_LKS0+0: case GE_LKS0+1: case GE_LKS0+2: case GE_LKS0+3:
                ge.light_spot_exp[cmd-GE_LKS0]=decode_float24(data); break;
            case GE_LKO0+0: case GE_LKO0+1: case GE_LKO0+2: case GE_LKO0+3:
                ge.light_spot_cut[cmd-GE_LKO0]=decode_float24(data); break;
            case GE_LAC0+0: case GE_LAC0+1: case GE_LAC0+2: case GE_LAC0+3:
            case GE_LAC0+4: case GE_LAC0+5: case GE_LAC0+6: case GE_LAC0+7:
            case GE_LAC0+8: case GE_LAC0+9: case GE_LAC0+10:case GE_LAC0+11: {
                int k=(int)(cmd-GE_LAC0), li=k/3;
                if      (k%3==0) ge.light_acol[li]=data;
                else if (k%3==1) ge.light_dcol[li]=data;
                else             ge.light_scol[li]=data;
                break;
            }

            /* ---- Texturing ---- */
            case GE_TEXTUREMAPENABLE: ge.tex_enable=data&1; break;
            case GE_TEXADDR0:     ge.tex_addr=(ge.tex_addr&0xFF000000)|(data&0x00FFFFFF); break;
            case GE_TEXBUFWIDTH0: ge.tex_bufw=data&0xFFFF; ge.tex_addr=(ge.tex_addr&0x00FFFFFF)|((data&0xFF0000)<<8); break;
            case GE_TEXSIZE0:  ge.tex_w=1<<(data&0xF); ge.tex_h=1<<((data>>8)&0xF); break;
            /* Mip levels 1-7 (addr/bufwidth/size): only level 0 is sampled. */
            case GE_TEXADDR0+1: case GE_TEXADDR0+2: case GE_TEXADDR0+3: case GE_TEXADDR0+4:
            case GE_TEXADDR0+5: case GE_TEXADDR0+6: case GE_TEXADDR0+7:
            case GE_TEXBUFWIDTH0+1: case GE_TEXBUFWIDTH0+2: case GE_TEXBUFWIDTH0+3:
            case GE_TEXBUFWIDTH0+4: case GE_TEXBUFWIDTH0+5: case GE_TEXBUFWIDTH0+6:
            case GE_TEXBUFWIDTH0+7:
            case GE_TEXSIZE0+1: case GE_TEXSIZE0+2: case GE_TEXSIZE0+3: case GE_TEXSIZE0+4:
            case GE_TEXSIZE0+5: case GE_TEXSIZE0+6: case GE_TEXSIZE0+7: break;
            case GE_TEXMODE:   ge.tex_swizzle=data&1; break;
            case GE_TEXFORMAT: ge.tex_fmt=data&0xF; break;
            case GE_TEXFILTER: ge.tex_filter=data; break;
            case GE_TEXWRAP:   ge.tex_wrap=data; break;
            case GE_TEXFUNC:   ge.tex_func=data; break;
            case GE_TEXENVCOLOR: ge.tex_env=data; break;
            case GE_TEXSCALEU:  ge.tex_scale_u=decode_float24(data); break;
            case GE_TEXSCALEV:  ge.tex_scale_v=decode_float24(data); break;
            case GE_TEXOFFSETU: ge.tex_off_u=decode_float24(data); break;
            case GE_TEXOFFSETV: ge.tex_off_v=decode_float24(data); break;
            case GE_TEXMAPMODE: ge.tex_map_mode=data; break;
            case GE_TEXSHADELS: ge.tex_shade_ls=data; break;
            case GE_TGENMATRIXNUMBER: ge.tgen_num = (int)(data & 0xF); break;
            case GE_TGENMATRIXDATA:
                if (ge.tgen_num < 12) ge.tgen[ge.tgen_num] = decode_float24(data);
                ge.tgen_num++;
                break;
            case GE_TEXLEVEL: case GE_TEXLODSLOPE: break;     /* mipmaps not sampled */
            case GE_TEXFLUSH: case GE_TEXSYNC: break;         /* no texture cache to flush */
            case GE_LOADCLUT: {
                uint64_t profile_started = ge_cpu_profile_begin();
                /* Copy (data&0x3F) 32-byte blocks from clut_addr into internal CLUT RAM, like
                 * the hardware (PPSSPP TextureCacheCommon::LoadClut). */
                /* CLUT sourced from VRAM may read pixels the GPU backend hasn't written back. */
                if (s_gpu && s_gpu->flush && (ge.clut_addr & 0x0F000000u) == 0x04000000u)
                    s_gpu->flush("loadclut");
                uint32_t bytes = (data & 0x3F) * 32;
                if (bytes > sizeof(ge.clutram)) bytes = sizeof(ge.clutram);
                for (uint32_t i = 0; i < bytes; i += 4) {
                    uint32_t w = MEM_R32(ge.clut_addr + i);
                    memcpy(&ge.clutram[i], &w, 4);
                }
                ge.clut_gen++;
                if (s_cpu_profile) s_cpu_profile_stats.clut_loads++;
                ge_cpu_profile_end(GE_CPU_CLUT_LOAD, profile_started);
                break;
            }
            case GE_CLUTADDR:      ge.clut_addr=(ge.clut_addr&0xFF000000)|(data&0x00FFFFFF); break;
            case GE_CLUTADDRUPPER: ge.clut_addr=(ge.clut_addr&0x00FFFFFF)|((data&0xFF0000)<<8); break;
            case GE_CLUTFORMAT: ge.clut_fmt=data; break;

            case GE_BLENDMODE:  ge.blend_mode=data; break;
            case GE_BLENDFIXEDA: ge.blend_fixa=data; break;
            case GE_BLENDFIXEDB: ge.blend_fixb=data; break;
            case GE_CLEARMODE:  ge.clear_mode=data; ge.clear=data&1; break;

            /* ---- Block transfer ---- */
            case GE_TRANSFERSRC:    ge.xf_src  = data; break;
            case GE_TRANSFERSRCW:   ge.xf_srcw = data; break;
            case GE_TRANSFERDST:    ge.xf_dst  = data; break;
            case GE_TRANSFERDSTW:   ge.xf_dstw = data; break;
            case GE_TRANSFERSRCPOS: ge.xf_spos = data; break;
            case GE_TRANSFERDSTPOS: ge.xf_dpos = data; break;
            case GE_TRANSFERSIZE:   ge.xf_size = data; break;
            case GE_TRANSFERSTART: {
                uint64_t profile_started = ge_cpu_profile_begin();
                ge_block_transfer(data);
                if (s_cpu_profile) s_cpu_profile_stats.block_transfers++;
                ge_cpu_profile_end(GE_CPU_BLOCK_TRANSFER, profile_started);
                break;
            }

            case GE_PRIM: {
                uint64_t profile_started = ge_cpu_profile_begin();
                prims++; draw_prim(op, prims);
                if (s_cpu_profile) {
                    s_cpu_profile_stats.primitive_commands++;
                    s_cpu_profile_stats.primitive_vertices += op & 0xFFFFu;
                }
                ge_cpu_profile_end(GE_CPU_PRIMITIVE, profile_started);
                break;
            }
            case GE_BEZIER: case GE_SPLINE: {
                uint64_t profile_started = ge_cpu_profile_begin();
                if (s_primitive_profile) {
                    s_cpu_profile_stats.primitive_profile_patch_commands++;
                    s_cpu_profile_stats.primitive_profile_patch_control_vertices +=
                        (uint64_t)(op & 0xFFu) * (uint64_t)((op >> 8) & 0xFFu);
                }
                prims++; draw_patch(op, cmd == GE_SPLINE);
                if (s_cpu_profile) s_cpu_profile_stats.primitive_commands++;
                ge_cpu_profile_end(GE_CPU_PRIMITIVE, profile_started);
                break;
            }
            case GE_JUMP: addr=ge_rel_addr(data&0x00FFFFFCu); break;
            case GE_BJUMP: break;  /* no bounding-box test result kept; draw everything */
            case GE_BOUNDINGBOX: break;
            case GE_CALL:
                if (s_sp<(int)(sizeof(s_stack_pc)/sizeof(s_stack_pc[0]))) {
                    s_stack_pc[s_sp]=addr; s_stack_offset[s_sp]=ge.offset; s_sp++;
                    addr=ge_rel_addr(data&0x00FFFFFCu);
                }
                break;
            case GE_RET:
                if (s_sp>0) { s_sp--; addr=s_stack_pc[s_sp]&0x0FFFFFFFu; ge.offset=s_stack_offset[s_sp]; }
                break;
            case GE_SIGNAL:
                /* SIGNAL semantics are selected by bits 16..23 and completed by the following
                 * END word.  Preserve the full word so the paired END can decode the behavior.
                 * SR_GESIGNALLOG reports each behavior a bounded number of times while the
                 * command-flow implementation is being validated against real game lists. */
                pending_signal=1; pending_signal_op=op; break;
            case GE_FINISH: pending_signal=0; break;  /* END that follows terminates the list */
            case GE_END:
                if (pending_signal) {
                    static unsigned char signal_seen[256];
                    unsigned behavior=(pending_signal_op>>16)&0xFFu;
                    static int s_gesiglog = -1;
                    if (s_gesiglog < 0) s_gesiglog = getenv("SR_GESIGNALLOG") ? 1 : 0;
                    if (s_gesiglog && signal_seen[behavior] < 8) {
                        signal_seen[behavior]++;
                        fprintf(stderr,
                                "GE SIGNAL/END: list=0x%08x pc=0x%08x behavior=0x%02x token=0x%04x end=0x%04x\n",
                                list_addr, addr-8, behavior, pending_signal_op&0xFFFFu, data&0xFFFFu);
                    }
                    pending_signal=0; pending_signal_op=0; break;
                }
                if (s_gpu && s_gpu->flush) {
                    uint64_t profile_started = ge_cpu_profile_begin();
                    s_gpu->flush("listend");
                    if (s_cpu_profile) s_cpu_profile_stats.flushes++;
                    ge_cpu_profile_end(GE_CPU_FLUSH, profile_started);
                }
                if (ge_capture_active()) s_capture_total_prims += prims;
                if (snap && g_tex_nonzero>start_nz) ge_snapshot("fb_content.ppm");
                g_ge_list_sig=sig; g_ge_prim_count=prims;
                if (s_gewatch > 0 && s_ge_frame >= (uint32_t)s_gewatch_after)
                    fprintf(stderr, "GELIST f=%u list=0x%08x fbp=0x%08x prims=%lu writes=%lu nonblack=%lu clearpx=%lu\n",
                            s_ge_frame, list_addr, ge_fb_addr(), prims,
                            g_list_writes, g_list_nonblack, g_list_clearpx);
                return 0;
            default:
                ge_note_unhandled_cmd(cmd, op);
                break;
        }
    }
    return addr;
}

uint32_t ge_framebuffer(void) { return ge_fb_addr(); }
