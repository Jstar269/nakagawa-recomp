// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include "ge_capture.h"
#include "ge_shared.h"
#include "recomp.h"
#include "gpu_sdl3vk/ge_gpu.h"
#include "gpu_sdl3vk/sdl3vk.h"

#include <SDL3/SDL_timer.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Standalone support required by recomp.h's checked memory accessors. The replay owns a
 * private guest arena and deliberately does not link generated game code or HLE state. */
uint8_t *g_mem;
uint32_t g_sr_debug;
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count;
int g_sr_metadata_watch;
int g_sr_heap_watch;
int g_hle_depth;
CpuState *s_cpu;

/* Store-context / last-writer diagnostic state referenced by ge.c. The replay links the
 * production GE object but not debug.c, so it supplies the same inert definitions that
 * gpu_coherence_selftest.c does. Without these the target does not link at all. */
int g_sr_last_writer_enabled;
uint32_t g_sr_store_context_pc;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset;
unsigned g_sr_store_context_mem_words;

void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}
void sr_add_mem_watch(uint32_t start, uint32_t end, const char *label) {
    (void)start; (void)end; (void)label;
}
int sr_find_last_writer(uint32_t addr, uint32_t width,
                        uint32_t *write_addr, uint32_t *write_width,
                        uint32_t *value, uint32_t *pc) {
    (void)addr; (void)width; (void)write_addr; (void)write_width;
    (void)value; (void)pc;
    return 0;
}

void sr_oor(uint32_t addr, uint32_t value, int store) {
    fprintf(stderr, "ge_replay: out-of-range %s addr=0x%08x value=0x%08x\n",
            store ? "write" : "read", addr, value);
}
void sr_heap_note_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    (void)addr; (void)width; (void)value; (void)pc;
}
void sr_heap_note_bulk_write(uint32_t addr, uint32_t width, uint32_t pc) {
    (void)addr; (void)width; (void)pc;
}
uint32_t sched_current_uid(void) { return 0; }
uint32_t sr_get_ge_status(void) { return 0; }

static uint32_t fb_addr(const GeState *state) {
    uint32_t addr = state->fbp;
    if ((addr & 0x0F000000u) == 0x04000000u) return addr;
    return 0x04000000u | (addr & 0x001FFFFFu);
}

static void unpack(uint32_t raw, uint32_t fmt, unsigned char rgb[3]) {
    uint32_t r, g, b;
    switch (fmt & 3u) {
        case 0: r = raw & 31u; g = (raw >> 5) & 63u; b = (raw >> 11) & 31u;
                r = (r << 3) | (r >> 2); g = (g << 2) | (g >> 4); b = (b << 3) | (b >> 2); break;
        case 1: r = raw & 31u; g = (raw >> 5) & 31u; b = (raw >> 10) & 31u;
                r = (r << 3) | (r >> 2); g = (g << 3) | (g >> 2); b = (b << 3) | (b >> 2); break;
        case 2: r = (raw & 15u) * 17u; g = ((raw >> 4) & 15u) * 17u; b = ((raw >> 8) & 15u) * 17u; break;
        default: r = raw & 255u; g = (raw >> 8) & 255u; b = (raw >> 16) & 255u; break;
    }
    rgb[0] = (unsigned char)r; rgb[1] = (unsigned char)g; rgb[2] = (unsigned char)b;
}

static int write_ppm(const char *path, const GeState *state) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    uint32_t base = fb_addr(state);
    uint32_t stride = state->fbw ? state->fbw : 512u;
    uint32_t bpp = (state->fbfmt & 3u) == 3u ? 4u : 2u;
    int ok = fprintf(file, "P6\n480 272\n255\n") > 0;
    for (uint32_t y = 0; ok && y < 272; y++) {
        for (uint32_t x = 0; x < 480; x++) {
            uint32_t addr = base + (y * stride + x) * bpp;
            uint32_t raw = bpp == 4 ? sr_r32(addr) : sr_r16(addr);
            unsigned char rgb[3];
            unpack(raw, state->fbfmt, rgb);
            if (fwrite(rgb, 1, sizeof(rgb), file) != sizeof(rgb)) { ok = 0; break; }
        }
    }
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static void usage(const char *exe) {
    fprintf(stderr, "usage: %s <fixture.ngef> [--backend software|vulkan] [--repeat N] [--output frame.ppm]\n"
                    "       %s <fixture.ngef> --first-bad-draw [--mode auto|linear|bisect] [--report PREFIX]\n"
                    "       %s --write-synthetic <fixture.ngef>\n"
                    "       %s --fbd-self-check\n", exe, exe, exe, exe);
}

/* ---- --first-bad-draw (#2141) ---------------------------------------------------------------
 * The software GE in ge.c is the Vulkan backend's reference (gpu_sdl3vk/README.md). This mode
 * replays a prefix of the capture's draws through each renderer in turn, in this one process,
 * and compares guest VRAM afterwards: every colour target, depth buffer and block-transfer
 * destination the prefix used. A prefix stops before a draw command (ge_set_draw_observer), so
 * the Vulkan backend batches and flushes exactly as in a full replay up to that point and
 * materializes there, as it does at every flush point.
 *
 * - linear: prefixes 0, 1, 2, ... until one differs; exact for any frame.
 * - bisect: halves the prefix, about log2(draws) replays per renderer. It finds a draw whose
 *   prefix differs while the prefix before it does not, which is the first bad draw when a
 *   difference persists to the end of the frame; a later draw can paint one over, and linear
 *   mode checks every prefix.
 * - auto (the default): linear up to FBD_AUTO_LINEAR_MAX draws, bisect above.
 *
 * A difference gpu_sdl3vk/README.md documents is counted under that rule's name, never as a
 * pass or a failure. One rule exists: "blend-2lsb", fixed-function blends differ from the
 * software blender's truncating products by at most 2 LSB. It covers a colour pixel only when
 * the prefix ran a blended draw, its alpha (stencil) is equal, and each colour channel is
 * within 2 LSB on an 8-bit channel or 1 LSB on a narrower one (a 2 LSB change of the 8-bit
 * result moves a truncated narrower channel by at most 1). Every other difference counts. */
#define FBD_AUTO_LINEAR_MAX 256ul
#define FBD_MAX_REGIONS 32
#define FBD_ROWS 272u

enum { FBD_KIND_COLOR, FBD_KIND_DEPTH, FBD_KIND_XFER };

typedef struct FbdRegion {
    int kind;
    uint32_t base;       /* guest address */
    uint32_t stride;     /* pixels per row */
    uint32_t fmt;        /* colour: GE pixel format 0-3; transfer: bytes per pixel */
    uint32_t width, rows;
} FbdRegion;

typedef struct FbdRun {
    unsigned long limit;
    unsigned long seen;
    int stopped;
    uint32_t list_index;
    int have_last;
    unsigned long last_index;
    uint32_t last_op, last_cmd_addr, last_list_addr, last_list_index;
    GeState last_state;
    FbdRegion regions[FBD_MAX_REGIONS];
    int nregions;
    int regions_dropped;
    int blended;
} FbdRun;

typedef struct FbdShot {
    uint32_t *px[FBD_MAX_REGIONS];
} FbdShot;

typedef struct FbdRegionDiff {
    unsigned long pixels, rule_pixels;
    uint32_t max_diff, rule_max_diff;
    uint32_t x0, y0, x1, y1;
} FbdRegionDiff;

typedef struct FbdDiff {
    unsigned long pixels, rule_pixels;
    FbdRegionDiff region[FBD_MAX_REGIONS];
} FbdDiff;

/* The draw that is running, for the test-only mutant hooks below. */
static unsigned long s_fbd_current = (unsigned long)-1;

static uint32_t fbd_bpp(const FbdRegion *r) {
    if (r->kind == FBD_KIND_DEPTH) return 2u;
    if (r->kind == FBD_KIND_XFER) return r->fmt;
    return r->fmt == 3u ? 4u : 2u;
}

static void fbd_add_region(FbdRun *run, int kind, uint32_t base, uint32_t stride, uint32_t fmt) {
    if (stride == 0u || stride > 1024u) return;
    for (int i = 0; i < run->nregions; i++) {
        const FbdRegion *r = &run->regions[i];
        if (r->kind == kind && r->base == base && r->stride == stride && r->fmt == fmt) return;
    }
    if (run->nregions == FBD_MAX_REGIONS) { run->regions_dropped++; return; }
    FbdRegion r = { kind, base, stride, fmt, stride < 480u ? stride : 480u, FBD_ROWS };
    uint32_t bpp = fbd_bpp(&r);
    /* Rows that lie in guest memory; a buffer near the end of VRAM keeps the rows it has. */
    while (r.rows && !sr_inrange_n(base, ((r.rows - 1u) * stride + r.width) * bpp)) r.rows--;
    if (!r.rows) return;
    run->regions[run->nregions++] = r;
}

static uint32_t fbd_depth_base(const GeState *st) {
    uint32_t base = st->zbp ? SR_PHYS(st->zbp) : SR_VRAM_CPU_BASE;
    return base < 0x00200000u ? base + SR_VRAM_CPU_BASE : base;
}

static int fbd_observer(void *ctx, unsigned long index, uint32_t op, uint32_t cmd_addr,
                        uint32_t list_addr) {
    FbdRun *run = (FbdRun *)ctx;
    if (index >= run->limit) { run->stopped = 1; return 1; }
    const GeState *st = ge_state_ptr();
    run->seen = index + 1u;
    run->have_last = 1;
    run->last_index = index;
    run->last_op = op;
    run->last_cmd_addr = cmd_addr;
    run->last_list_addr = list_addr;
    run->last_list_index = run->list_index;
    run->last_state = *st;
    s_fbd_current = index;
    if ((op >> 24) == 0xEAu) {   /* TRANSFERSTART: the destination rectangle's buffer */
        uint32_t dst = (st->xf_dst & 0xFFFFF0u) | ((st->xf_dstw & 0xFF0000u) << 8);
        dst = dst < 0x00200000u ? dst | SR_VRAM_CPU_BASE : dst;
        fbd_add_region(run, FBD_KIND_XFER, dst, st->xf_dstw & 0x7F8u, (op & 1u) ? 4u : 2u);
        return 0;
    }
    fbd_add_region(run, FBD_KIND_COLOR, fb_addr(st), st->fbw, st->fbfmt & 3u);
    if (st->zbw && st->zbw <= 512u)
        fbd_add_region(run, FBD_KIND_DEPTH, fbd_depth_base(st), st->zbw, 0u);
    if (st->blend_enable && !st->clear) run->blended = 1;
    return 0;
}

/* Depth sits in VRAM with the 0x2040 address swizzle ge.c's ge_zbuf_guest_address applies. */
static uint32_t fbd_addr(const FbdRegion *r, uint32_t x, uint32_t y) {
    uint32_t addr = r->base + (y * r->stride + x) * fbd_bpp(r);
    return r->kind == FBD_KIND_DEPTH ? addr ^ 0x2040u : addr;
}

static uint32_t fbd_load(const FbdRegion *r, uint32_t x, uint32_t y) {
    uint32_t addr = fbd_addr(r, x, y);
    if (!sr_inrange_n(addr, fbd_bpp(r))) return 0;
    if (fbd_bpp(r) == 4u) { uint32_t v; memcpy(&v, SR_HOST_LINEAR(addr), 4); return v; }
    uint16_t v; memcpy(&v, SR_HOST_LINEAR(addr), 2); return v;
}

static int fbd_snapshot(const FbdRun *regions, FbdShot *shot) {
    for (int i = 0; i < regions->nregions; i++) {
        const FbdRegion *r = &regions->regions[i];
        shot->px[i] = (uint32_t *)malloc((size_t)r->width * r->rows * sizeof(uint32_t));
        if (!shot->px[i]) return 0;
        for (uint32_t y = 0; y < r->rows; y++)
            for (uint32_t x = 0; x < r->width; x++)
                shot->px[i][y * r->width + x] = fbd_load(r, x, y);
    }
    return 1;
}

static void fbd_shot_free(FbdShot *shot) {
    for (int i = 0; i < FBD_MAX_REGIONS; i++) { free(shot->px[i]); shot->px[i] = NULL; }
}

/* Channel layout of a region's words: shift and width of up to four channels; the last
 * channel of a colour format with alpha is alpha (stencil). */
typedef struct FbdLayout { int n; int alpha; uint8_t shift[4], bits[4]; } FbdLayout;

static FbdLayout fbd_layout(const FbdRegion *r) {
    static const FbdLayout color[4] = {
        { 3, -1, { 0, 5, 11, 0 }, { 5, 6, 5, 0 } },
        { 4, 3, { 0, 5, 10, 15 }, { 5, 5, 5, 1 } },
        { 4, 3, { 0, 4, 8, 12 }, { 4, 4, 4, 4 } },
        { 4, 3, { 0, 8, 16, 24 }, { 8, 8, 8, 8 } },
    };
    if (r->kind == FBD_KIND_COLOR) return color[r->fmt & 3u];
    FbdLayout one = { 1, -1, { 0, 0, 0, 0 }, { (uint8_t)(fbd_bpp(r) * 8u), 0, 0, 0 } };
    return one;
}

/* Classifies one differing pixel: returns 1 when blend-2lsb covers it. *diff gets the
 * largest per-channel difference in the region's own units. */
static int fbd_classify(const FbdRegion *r, uint32_t a, uint32_t b, int blended, uint32_t *diff) {
    FbdLayout l = fbd_layout(r);
    int covered = blended && r->kind == FBD_KIND_COLOR;
    *diff = 0;
    for (int c = 0; c < l.n; c++) {
        uint32_t mask = l.bits[c] >= 32 ? 0xFFFFFFFFu : ((1u << l.bits[c]) - 1u);
        uint32_t ca = (a >> l.shift[c]) & mask, cb = (b >> l.shift[c]) & mask;
        uint32_t d = ca > cb ? ca - cb : cb - ca;
        if (d > *diff) *diff = d;
        if (c == l.alpha) { if (d) covered = 0; }
        else if (d > (l.bits[c] >= 8 ? 2u : 1u)) covered = 0;
    }
    return covered;
}

static void fbd_compare(const FbdRun *regions, const FbdShot *sw, const FbdShot *vk, FbdDiff *out) {
    memset(out, 0, sizeof(*out));
    for (int i = 0; i < regions->nregions; i++) {
        const FbdRegion *r = &regions->regions[i];
        FbdRegionDiff *d = &out->region[i];
        d->x0 = d->y0 = UINT32_MAX;
        for (uint32_t y = 0; y < r->rows; y++) {
            for (uint32_t x = 0; x < r->width; x++) {
                uint32_t a = sw->px[i][y * r->width + x], b = vk->px[i][y * r->width + x];
                if (a == b) continue;
                uint32_t diff;
                if (fbd_classify(r, a, b, regions->blended, &diff)) {
                    d->rule_pixels++;
                    if (diff > d->rule_max_diff) d->rule_max_diff = diff;
                    continue;
                }
                d->pixels++;
                if (diff > d->max_diff) d->max_diff = diff;
                if (x < d->x0) d->x0 = x;
                if (y < d->y0) d->y0 = y;
                if (x > d->x1) d->x1 = x;
                if (y > d->y1) d->y1 = y;
            }
        }
        out->pixels += d->pixels;
        out->rule_pixels += d->rule_pixels;
    }
}

/* One renderer, one prefix. hooks NULL is ge.c's software rasterizer. `regions` NULL records
 * the prefix's buffers into run; otherwise the snapshot covers those regions. */
static int fbd_pass(const GeCaptureFixture *fixture, const GeGpuHooks *hooks, unsigned long limit,
                    FbdRun *run, const FbdRun *regions, FbdShot *shot) {
    ge_set_gpu_hooks(hooks);
    if (hooks && !gegpu_replay_reset()) return 0;
    memset(run, 0, sizeof(*run));
    run->limit = limit;
    if (!ge_capture_apply(fixture, ge_state_ptr(), ge_zbuf_ptr())) return 0;
    ge_replay_restore(&fixture->initial_state, fixture->initial_zbuf);
    ge_set_draw_observer(fbd_observer, run);
    int ok = 1;
    for (uint32_t list = 0; ok && list < fixture->list_count; list++) {
        run->list_index = list;
        uint32_t next = ge_run_list(fixture->list_addrs[list], 0);
        if (run->stopped) break;
        if (next != 0) {
            fprintf(stderr, "ge_replay: list %u did not terminate\n", list);
            ok = 0;
        }
    }
    ge_set_draw_observer(NULL, NULL);
    s_fbd_current = (unsigned long)-1;
    if (ok && hooks && !gegpu_capture_materialize()) ok = 0;
    if (ok && !ge_zbuf_materialize_current()) ok = 0;
    if (ok) ok = fbd_snapshot(regions ? regions : run, shot);
    return ok;
}

/* Evaluates prefix `limit`: software, then Vulkan. Returns -1 on a replay failure, else the
 * count of differing pixels no rule covers. */
static long fbd_eval(const GeCaptureFixture *fixture, const GeGpuHooks *vk_hooks,
                     unsigned long limit, FbdRun *sw_run, FbdShot *sw, FbdShot *vk, FbdDiff *diff) {
    static FbdRun vk_run;
    fbd_shot_free(sw);
    fbd_shot_free(vk);
    if (!fbd_pass(fixture, NULL, limit, sw_run, NULL, sw)) return -1;
    if (!fbd_pass(fixture, vk_hooks, limit, &vk_run, sw_run, vk)) return -1;
    if (vk_run.seen != sw_run->seen) {
        fprintf(stderr, "ge_replay: the renderers ran %lu and %lu draws of prefix %lu\n",
                sw_run->seen, vk_run.seen, limit);
        return -1;
    }
    fbd_compare(sw_run, sw, vk, diff);
    return diff->pixels > (unsigned long)LONG_MAX ? LONG_MAX : (long)diff->pixels;
}

static const char *fbd_cmd_name(uint32_t op) {
    switch (op >> 24) {
        case 0x04: return "PRIM";
        case 0x05: return "BEZIER";
        case 0x06: return "SPLINE";
        case 0xF7: return "VAP";
        case 0xEA: return "TRANSFERSTART";
        default: return "OTHER";
    }
}

static const char *fbd_kind_name(int kind) {
    return kind == FBD_KIND_COLOR ? "color" : kind == FBD_KIND_DEPTH ? "depth" : "transfer";
}

/* An 8-bit RGB view of one word, for the crops. */
static void fbd_rgb(const FbdRegion *r, uint32_t raw, unsigned char rgb[3]) {
    if (r->kind == FBD_KIND_COLOR) { unpack(raw, r->fmt, rgb); return; }
    unsigned char v = (unsigned char)(fbd_bpp(r) == 4u ? raw >> 24 : raw >> 8);
    rgb[0] = rgb[1] = rgb[2] = v;
}

static int fbd_write_crop(const char *path, const FbdRegion *r, const uint32_t *a,
                          const uint32_t *b, int mode, uint32_t x0, uint32_t y0,
                          uint32_t x1, uint32_t y1) {
    FILE *file = fopen(path, "wb");
    if (!file) return 0;
    int ok = fprintf(file, "P6\n%u %u\n255\n", x1 - x0 + 1u, y1 - y0 + 1u) > 0;
    for (uint32_t y = y0; ok && y <= y1; y++) {
        for (uint32_t x = x0; ok && x <= x1; x++) {
            unsigned char rgb[3];
            uint32_t va = a[y * r->width + x], vb = b[y * r->width + x];
            if (mode == 0) fbd_rgb(r, va, rgb);
            else if (mode == 1) fbd_rgb(r, vb, rgb);
            else {   /* differing pixels white, equal ones a dim copy of the software image */
                fbd_rgb(r, va, rgb);
                for (int c = 0; c < 3; c++) rgb[c] = va == vb ? (unsigned char)(rgb[c] / 4) : 255;
            }
            ok = fwrite(rgb, 1, 3, file) == 3;
        }
    }
    if (fclose(file) != 0) ok = 0;
    return ok;
}

static void fbd_print_regions(const FbdRun *run, const FbdDiff *diff) {
    for (int i = 0; i < run->nregions; i++) {
        const FbdRegion *r = &run->regions[i];
        const FbdRegionDiff *d = &diff->region[i];
        if (!d->pixels && !d->rule_pixels) continue;
        printf("GE_FBD_REGION kind=%s base=0x%08x stride=%u fmt=%u pixels=%lu max_diff=%u",
               fbd_kind_name(r->kind), r->base, r->stride, r->fmt, d->pixels, d->max_diff);
        if (d->pixels) printf(" bbox=%u,%u,%u,%u", d->x0, d->y0, d->x1, d->y1);
        if (d->rule_pixels) printf(" rule=blend-2lsb rule_pixels=%lu rule_max_diff=%u",
                                   d->rule_pixels, d->rule_max_diff);
        putchar('\n');
    }
    if (diff->rule_pixels)
        printf("GE_FBD_RULE rule=blend-2lsb pixels=%lu source=src/rt/gpu_sdl3vk/README.md\n",
               diff->rule_pixels);
    if (run->regions_dropped)
        printf("GE_FBD_WARNING regions_not_compared=%d\n", run->regions_dropped);
}

static void fbd_print_draw(const FbdRun *run) {
    const GeState *st = &run->last_state;
    uint32_t op = run->last_op;
    printf("GE_FBD_DRAW index=%lu command=%s op=0x%08x list=%u list_addr=0x%08x cmd_addr=0x%08x "
           "list_offset=0x%x",
           run->last_index, fbd_cmd_name(op), op, run->last_list_index, run->last_list_addr,
           run->last_cmd_addr, run->last_cmd_addr - run->last_list_addr);
    if ((op >> 24) == 0x04u) printf(" prim=%u count=%u", (op >> 16) & 7u, op & 0xFFFFu);
    printf(" vtype=0x%06x through=%u clear=%d clear_mode=0x%x fbp=0x%08x fbw=%u fbfmt=%u "
           "zbp=0x%08x zbw=%u tex=%d tex_fmt=%u tex_addr=0x%08x tex_func=0x%x "
           "blend=%d blend_mode=0x%06x fixa=0x%06x fixb=0x%06x "
           "ztest=%d zfunc=%u zwrite_disable=%d atest=%d atest_word=0x%06x "
           "stencil=%d stencil_test=0x%06x stencil_op=0x%06x dither=%d logic_op=%d "
           "mask_rgb=0x%06x mask_alpha=0x%02x\n",
           st->vtype, (st->vtype >> 23) & 1u, st->clear, st->clear_mode, fb_addr(st), st->fbw,
           st->fbfmt, st->zbp, st->zbw, st->tex_enable, st->tex_fmt, st->tex_addr, st->tex_func,
           st->blend_enable, st->blend_mode, st->blend_fixa, st->blend_fixb,
           st->ztest_enable, st->ztest, st->zwrite_disable, st->atest_enable, st->atest,
           st->stencil_enable, st->stencil_test, st->stencil_op, st->dither_enable,
           st->logic_op_enable, st->mask_rgb, (unsigned)st->mask_alpha & 0xFFu);
}

/* Crops of the region with the most uncovered pixels: software, Vulkan and a difference map,
 * written as <prefix>.software.ppm, .vulkan.ppm and .diff.ppm. */
static void fbd_write_crops(const char *prefix, const FbdRun *run, const FbdShot *sw,
                            const FbdShot *vk, const FbdDiff *diff) {
    int best = -1;
    for (int i = 0; i < run->nregions; i++)
        if (diff->region[i].pixels &&
            (best < 0 || diff->region[i].pixels > diff->region[best].pixels))
            best = i;
    if (best < 0) return;
    const FbdRegion *r = &run->regions[best];
    const FbdRegionDiff *d = &diff->region[best];
    uint32_t x0 = d->x0 > 8u ? d->x0 - 8u : 0u, y0 = d->y0 > 8u ? d->y0 - 8u : 0u;
    uint32_t x1 = d->x1 + 8u < r->width ? d->x1 + 8u : r->width - 1u;
    uint32_t y1 = d->y1 + 8u < r->rows ? d->y1 + 8u : r->rows - 1u;
    static const char *names[3] = { "software", "vulkan", "diff" };
    for (int mode = 0; mode < 3; mode++) {
        char path[1024];
        snprintf(path, sizeof path, "%s.%s.ppm", prefix, names[mode]);
        if (fbd_write_crop(path, r, sw->px[best], vk->px[best], mode, x0, y0, x1, y1))
            printf("GE_FBD_CROP image=%s region=%s base=0x%08x rect=%u,%u,%u,%u path=%s\n",
                   names[mode], fbd_kind_name(r->kind), r->base, x0, y0, x1, y1, path);
        else
            fprintf(stderr, "ge_replay: could not write %s\n", path);
    }
}

#ifdef SR_GE_FBD_MUTANT
/* Test-only executed mutants for --first-bad-draw (make ge-first-bad-draw-selftest): the
 * Vulkan backend draws one draw with a changed state, so the renderers differ from that draw
 * on and the tool must name it. 1 = a blend the draw does not have (FIX 0x808080 on each
 * side), 2 = depth writes off. */
#ifndef SR_GE_FBD_MUTANT_DRAW
#define SR_GE_FBD_MUTANT_DRAW 3
#endif
static const GeGpuHooks *s_mut_real;
static GeGpuHooks s_mut_hooks;
static int s_mut_blend, s_mut_zwrite;
static uint32_t s_mut_mode, s_mut_fixa, s_mut_fixb;

static int mut_begin(void) {
    if (s_fbd_current != (unsigned long)SR_GE_FBD_MUTANT_DRAW) return 0;
    GeState *st = ge_state_ptr();
    s_mut_blend = st->blend_enable; s_mut_mode = st->blend_mode;
    s_mut_fixa = st->blend_fixa; s_mut_fixb = st->blend_fixb; s_mut_zwrite = st->zwrite_disable;
#if SR_GE_FBD_MUTANT == 1
    st->blend_enable = 1;
    st->blend_mode = 10u | (10u << 4);
    st->blend_fixa = st->blend_fixb = 0x808080u;
#elif SR_GE_FBD_MUTANT == 2
    st->zwrite_disable = 1;
#else
#error unknown SR_GE_FBD_MUTANT
#endif
    ge_state_note_external_write();
    return 1;
}

static void mut_end(int on) {
    if (!on) return;
    GeState *st = ge_state_ptr();
    st->blend_enable = s_mut_blend; st->blend_mode = s_mut_mode;
    st->blend_fixa = s_mut_fixa; st->blend_fixb = s_mut_fixb; st->zwrite_disable = s_mut_zwrite;
    ge_state_note_external_write();
}

static int mut_tri(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp) {
    int on = mut_begin(); int r = s_mut_real->tri(a, b, c, persp); mut_end(on); return r;
}
static int mut_sprite(const GeVtx *p0, const GeVtx *p1, int persp) {
    int on = mut_begin(); int r = s_mut_real->sprite(p0, p1, persp); mut_end(on); return r;
}
static int mut_tnl_prim(const struct GeTnlPrim *p) {
    int on = mut_begin(); int r = s_mut_real->tnl_prim(p); mut_end(on); return r;
}

static const GeGpuHooks *fbd_vulkan_hooks(void) {
    s_mut_real = ge_get_gpu_hooks();
    if (!s_mut_real) return NULL;
    s_mut_hooks = *s_mut_real;
    if (s_mut_hooks.tri) s_mut_hooks.tri = mut_tri;
    if (s_mut_hooks.sprite) s_mut_hooks.sprite = mut_sprite;
    if (s_mut_hooks.tnl_prim) s_mut_hooks.tnl_prim = mut_tnl_prim;
    printf("GE_FBD_MUTANT mutant=%d draw=%d\n", SR_GE_FBD_MUTANT, SR_GE_FBD_MUTANT_DRAW);
    return &s_mut_hooks;
}
#else
static const GeGpuHooks *fbd_vulkan_hooks(void) { return ge_get_gpu_hooks(); }
#endif

/* Returns 0 for NO_DIFFERENCE or RULE_ONLY, 1 for a first bad draw (or INITIAL_STATE), 3 when
 * a replay fails. */
static int fbd_run(const GeCaptureFixture *fixture, const char *mode, const char *report) {
    const GeGpuHooks *vk_hooks = fbd_vulkan_hooks();
    if (!vk_hooks) {
        fprintf(stderr, "ge_replay: the Vulkan backend registered no hooks\n");
        return 3;
    }
    static FbdRun sw_run, bad_run;
    static FbdShot sw, vk, bad_sw, bad_vk;
    static FbdDiff diff, bad_diff;
    /* The whole frame first: its draw count, and the answer when nothing differs. */
    long full = fbd_eval(fixture, vk_hooks, (unsigned long)-1, &sw_run, &sw, &vk, &diff);
    if (full < 0) return 3;
    unsigned long draws = sw_run.seen;
    int linear = strcmp(mode, "linear") == 0 ||
                 (strcmp(mode, "auto") == 0 && draws <= FBD_AUTO_LINEAR_MAX);
    const char *ran = linear ? "linear" : "bisect";
    unsigned long first_bad = (unsigned long)-1;   /* the smallest bad prefix length */
    unsigned long prefixes = 1;
    if (linear) {
        for (unsigned long n = 0; n <= draws; n++) {
            long bad = full;
            if (n != draws) {
                bad = fbd_eval(fixture, vk_hooks, n, &bad_run, &bad_sw, &bad_vk, &bad_diff);
                prefixes++;
            }
            if (bad < 0) return 3;
            if (bad > 0) { first_bad = n; break; }
        }
    } else if (full > 0) {
        unsigned long lo = 0, hi = draws;   /* prefix hi is bad; prefix lo is good once checked */
        long bad0 = fbd_eval(fixture, vk_hooks, 0, &bad_run, &bad_sw, &bad_vk, &bad_diff);
        prefixes++;
        if (bad0 < 0) return 3;
        if (bad0 > 0) hi = 0;
        while (hi > lo + 1u) {
            unsigned long mid = lo + (hi - lo) / 2u;
            long bad = fbd_eval(fixture, vk_hooks, mid, &bad_run, &bad_sw, &bad_vk, &bad_diff);
            prefixes++;
            if (bad < 0) return 3;
            if (bad > 0) hi = mid; else lo = mid;
        }
        first_bad = hi;
    }
    if (first_bad == (unsigned long)-1) {
        printf("GE_FIRST_BAD_DRAW result=%s draws=%lu mode=%s prefixes=%lu rule_pixels=%lu\n",
               diff.rule_pixels ? "RULE_ONLY" : "NO_DIFFERENCE", draws, ran, prefixes,
               diff.rule_pixels);
        fbd_print_regions(&sw_run, &diff);
        return 0;
    }
    /* The bad prefix's own replay, for the report and the crops. */
    FbdRun *run = &sw_run;
    FbdShot *a = &sw, *b = &vk;
    FbdDiff *d = &diff;
    if (first_bad != draws) {
        if (fbd_eval(fixture, vk_hooks, first_bad, &bad_run, &bad_sw, &bad_vk, &bad_diff) <= 0) {
            fprintf(stderr, "ge_replay: prefix %lu did not reproduce its difference\n", first_bad);
            return 3;
        }
        run = &bad_run; a = &bad_sw; b = &bad_vk; d = &bad_diff;
    }
    if (first_bad == 0) {
        printf("GE_FIRST_BAD_DRAW result=INITIAL_STATE draws=%lu mode=%s prefixes=%lu pixels=%lu\n",
               draws, ran, prefixes, d->pixels);
    } else {
        printf("GE_FIRST_BAD_DRAW result=FIRST_BAD_DRAW index=%lu draws=%lu mode=%s prefixes=%lu "
               "pixels=%lu rule_pixels=%lu\n",
               first_bad - 1u, draws, ran, prefixes, d->pixels, d->rule_pixels);
        fbd_print_draw(run);
    }
    if (!linear)
        printf("GE_FBD_NOTE bisect assumes a difference persists to the end of the frame; "
               "--mode linear checks every prefix\n");
    fbd_print_regions(run, d);
    fbd_write_crops(report, run, a, b, d);
    return 1;
}

/* The comparison rules on hand-made pixels, without a GPU (make ge-first-bad-draw-selftest). */
static int fbd_self_check(void) {
    int fails = 0;
    FbdRegion c8888 = { FBD_KIND_COLOR, 0, 512, 3, 480, 272 };
    FbdRegion c5650 = { FBD_KIND_COLOR, 0, 512, 0, 480, 272 };
    FbdRegion depth = { FBD_KIND_DEPTH, 0, 512, 0, 480, 272 };
    uint32_t diff;
#define FBD_EXPECT(cond, what) \
    do { if (!(cond)) { fails++; fprintf(stderr, "FAIL %s\n", what); } } while (0)
    FBD_EXPECT(fbd_classify(&c8888, 0xFF102030u, 0xFF121E31u, 1, &diff) == 1 && diff == 2,
               "8888: 2 LSB per channel after a blended draw is blend-2lsb");
    FBD_EXPECT(fbd_classify(&c8888, 0xFF102030u, 0xFF132030u, 1, &diff) == 0 && diff == 3,
               "8888: 3 LSB is a difference");
    FBD_EXPECT(fbd_classify(&c8888, 0xFF102030u, 0xFF121E31u, 0, &diff) == 0,
               "8888: without a blended draw no rule applies");
    FBD_EXPECT(fbd_classify(&c8888, 0xFF102030u, 0xFE102030u, 1, &diff) == 0 && diff == 1,
               "8888: an alpha (stencil) difference is never covered");
    FBD_EXPECT(fbd_classify(&c5650, 0x0001u, 0x0000u, 1, &diff) == 1,
               "5650: 1 LSB of a 5-bit channel is blend-2lsb");
    FBD_EXPECT(fbd_classify(&c5650, 0x0002u, 0x0000u, 1, &diff) == 0 && diff == 2,
               "5650: 2 LSB of a 5-bit channel is a difference");
    FBD_EXPECT(fbd_classify(&depth, 0x1000u, 0x1001u, 1, &diff) == 0 && diff == 1,
               "depth: any difference counts");
#undef FBD_EXPECT
    if (fails) return 1;
    puts("ge first-bad-draw self-check: OK");
    return 0;
}

/* A synthetic capture for the selftest, generated bytes only: one through-mode list on an
 * 8888 target with depth, a clear and five opaque sprites. Draw 3 is the mutants' draw, and
 * draw 4 covers part of it. */
static void fbd_put32(uint32_t addr, uint32_t value) { memcpy(SR_HOST_LINEAR(addr), &value, 4); }
static void fbd_putf(uint32_t addr, float value) { memcpy(SR_HOST_LINEAR(addr), &value, 4); }

static int fbd_write_synthetic(const char *path) {
    const uint32_t list = 0x08800000u, verts = 0x08810000u, init = 0x08820000u;
    /* ge.c's own power-on state: run an empty list once. */
    fbd_put32(init, 0x0F000000u);
    fbd_put32(init + 4u, 0x0C000000u);
    if (ge_run_list(init, 0) != 0) return 0;
    GeState initial = *ge_state_ptr();
    static const struct { float x0, y0, x1, y1, z; uint32_t abgr; } sprites[6] = {
        { 0, 0, 480, 272, 0, 0xFF202020u },                 /* 0: clear colour and depth */
        { 40, 40, 200, 160, 30000, 0xFF0000E0u },           /* 1 */
        { 120, 100, 320, 220, 20000, 0xFF00E000u },         /* 2: partly behind 1 */
        { 250, 30, 440, 140, 40000, 0xFFE00000u },          /* 3: the mutants' draw */
        { 300, 120, 460, 250, 50000, 0x8000E0E0u },         /* 4: over part of 3 */
        { 10, 180, 110, 260, 10000, 0xFFE0E000u },          /* 5 */
    };
    for (unsigned i = 0; i < 6; i++) {
        uint32_t v = verts + i * 32u;
        fbd_put32(v, sprites[i].abgr);
        fbd_putf(v + 4u, sprites[i].x0);
        fbd_putf(v + 8u, sprites[i].y0);
        fbd_putf(v + 12u, sprites[i].z);
        fbd_put32(v + 16u, sprites[i].abgr);
        fbd_putf(v + 20u, sprites[i].x1);
        fbd_putf(v + 24u, sprites[i].y1);
        fbd_putf(v + 28u, sprites[i].z);
    }
    const uint32_t sprite = (0x04u << 24) | (6u << 16) | 2u;
    const uint32_t cmds[] = {
        0x9C000000u, 0x9D000000u | 512u, 0xD2000003u,           /* FBP 0x04000000, 512, 8888 */
        0x9E088000u, 0x9F000000u | 512u,                        /* ZBP 0x04088000, 512 */
        0xD4000000u, 0xD5000000u | 479u | (271u << 10),         /* scissor 480x272 */
        0x12000000u | (1u << 23) | (7u << 2) | (3u << 7),       /* through, 8888 colour, float xyz */
        0x10000000u | ((verts >> 24) << 16), 0x01000000u | (verts & 0xFFFFFFu),
        0xD3000701u, sprite, 0xD3000000u,                       /* 0: clear */
        0x23000001u, 0xDE000007u, 0xE7000000u,                  /* depth test GEQUAL, writes on */
        sprite, sprite, sprite, sprite, sprite,                 /* 1-5 */
        0x0F000000u, 0x0C000000u,
    };
    for (unsigned i = 0; i < sizeof cmds / sizeof cmds[0]; i++) fbd_put32(list + i * 4u, cmds[i]);
    memset(SR_HOST_LINEAR(GE_CAPTURE_VRAM_BASE), 0, GE_CAPTURE_VRAM_SIZE);
    static uint16_t zbuf[GE_CAPTURE_ZBUF_WORDS];
    if (!ge_capture_begin(path, 1, list, &initial, zbuf)) return 0;
    ge_capture_note_memory(list, (uint32_t)sizeof cmds);
    ge_capture_note_memory(verts, 6u * 32u);
    return ge_capture_end(&initial);
}

typedef enum ReplayWallPhase {
    REPLAY_WALL_RESET = 0,
    REPLAY_WALL_FIXTURE_APPLY,
    REPLAY_WALL_GE_RESTORE,
    REPLAY_WALL_GE_LISTS,
    REPLAY_WALL_LOOP_OTHER,
    REPLAY_WALL_MATERIALIZE,
    REPLAY_WALL_PHASE_COUNT,
} ReplayWallPhase;

typedef struct ReplayWallPhaseStats {
    uint64_t calls;
    uint64_t ns;
} ReplayWallPhaseStats;

static int env_enabled(const char *name) {
    const char *value = getenv(name);
    return value && value[0] && strcmp(value, "0") != 0;
}

static uint64_t wall_phase_begin(int enabled) {
    return enabled ? SDL_GetTicksNS() : 0;
}

static void wall_phase_end(ReplayWallPhaseStats *phase, int enabled, uint64_t started) {
    if (!enabled) return;
    phase->calls++;
    phase->ns += SDL_GetTicksNS() - started;
}

static uint64_t primitive_profile_estimate_ns(uint64_t raw_ns, uint64_t samples,
                                              uint64_t eligible) {
    if (!raw_ns || !samples || !eligible) return 0;
    long double scaled = (long double)raw_ns * (long double)eligible / (long double)samples;
    if (scaled >= (long double)UINT64_MAX) return UINT64_MAX;
    return (uint64_t)scaled;
}

/* The empty control is a separate interval, so subtracting it from a phase or
 * sampled-total interval would double-count work that is not inside that
 * interval.  Keep an explicit adjusted value for reports, equal to the raw
 * estimate until a future design puts the control machinery inside the timed
 * interval. */
static uint64_t primitive_profile_adjusted_estimate_ns(uint64_t raw_ns, uint64_t samples,
                                                       uint64_t eligible) {
    return primitive_profile_estimate_ns(raw_ns, samples, eligible);
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(argv[0]); return 2; }
    if (strcmp(argv[1], "--fbd-self-check") == 0) return argc == 2 ? fbd_self_check() : 2;
    if (strcmp(argv[1], "--write-synthetic") == 0) {
        if (argc != 3) { usage(argv[0]); return 2; }
        uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
        if (!arena) return 2;
        g_mem = arena + 0x08000000u;
        int ok = fbd_write_synthetic(argv[2]);
        free(arena);
        if (!ok) { fprintf(stderr, "ge_replay: could not write %s\n", argv[2]); return 3; }
        printf("GE_REPLAY_SYNTHETIC path=%s\n", argv[2]);
        return 0;
    }
    const char *fixture_path = argv[1];
    const char *backend = "software";
    const char *output = "ge_replay.ppm";
    const char *fbd_mode = NULL;
    const char *fbd_report = NULL;
    unsigned long repeat = 1;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--backend") == 0 && i + 1 < argc) backend = argv[++i];
        else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) repeat = strtoul(argv[++i], NULL, 10);
        else if (strcmp(argv[i], "--output") == 0 && i + 1 < argc) output = argv[++i];
        else if (strcmp(argv[i], "--first-bad-draw") == 0) { if (!fbd_mode) fbd_mode = "auto"; }
        else if (strcmp(argv[i], "--mode") == 0 && i + 1 < argc) fbd_mode = argv[++i];
        else if (strcmp(argv[i], "--report") == 0 && i + 1 < argc) fbd_report = argv[++i];
        else { usage(argv[0]); return 2; }
    }
    if (fbd_mode) {
        if (strcmp(fbd_mode, "auto") != 0 && strcmp(fbd_mode, "linear") != 0 &&
            strcmp(fbd_mode, "bisect") != 0) { usage(argv[0]); return 2; }
        backend = "vulkan";
    }
    int vulkan = strcmp(backend, "vulkan") == 0;
    if ((!vulkan && strcmp(backend, "software") != 0) || repeat == 0 || repeat > 1000000ul) {
        usage(argv[0]); return 2;
    }

    GeCaptureFixture fixture;
    if (!ge_capture_load(fixture_path, &fixture)) {
        fprintf(stderr, "ge_replay: invalid fixture %s\n", fixture_path);
        return 2;
    }
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    if (!arena) { ge_capture_free(&fixture); return 2; }
    g_mem = arena + 0x08000000u;
    sr_perf_init();

    if (vulkan) {
        if (!sdl3vk_init("Nakagawa GE replay") || !gegpu_init()) {
            fprintf(stderr, "ge_replay: Vulkan initialization failed\n");
            ge_capture_free(&fixture); free(arena);
            /* --first-bad-draw has nothing to compare without Vulkan: a skip, not a pass. */
            if (fbd_mode) { puts("GE_FIRST_BAD_DRAW result=SKIP reason=no-vulkan"); return 77; }
            return 2;
        }
        gegpu_replay_stats_reset();
    }
    if (fbd_mode) {
        char report[1024];
        if (fbd_report) snprintf(report, sizeof report, "%s", fbd_report);
        else snprintf(report, sizeof report, "%s.first-bad-draw", fixture_path);
        int rc = fbd_run(&fixture, fbd_mode, report);
        gegpu_shutdown(); sdl3vk_shutdown();
        ge_capture_free(&fixture);
        free(arena);
        return rc;
    }
    ge_cpu_profile_reset();

    int wall_profile = env_enabled("SR_GE_REPLAY_WALL_PROFILE") ||
                       env_enabled("SR_GPU_CPU_PROFILE");
    ReplayWallPhaseStats wall_stats[REPLAY_WALL_PHASE_COUNT] = {{0}};
    uint64_t prior_assigned_ns = 0;
    uint64_t started = SDL_GetTicksNS();
    for (unsigned long i = 0; i < repeat; i++) {
        uint64_t frame_started = wall_phase_begin(wall_profile);
        uint64_t phase_started = wall_phase_begin(wall_profile);
        if (vulkan && !gegpu_replay_reset()) return 3;
        wall_phase_end(&wall_stats[REPLAY_WALL_RESET], wall_profile, phase_started);
        phase_started = wall_phase_begin(wall_profile);
        if (!ge_capture_apply(&fixture, ge_state_ptr(), ge_zbuf_ptr())) return 3;
        wall_phase_end(&wall_stats[REPLAY_WALL_FIXTURE_APPLY], wall_profile, phase_started);
        phase_started = wall_phase_begin(wall_profile);
        ge_replay_restore(&fixture.initial_state, fixture.initial_zbuf);
        wall_phase_end(&wall_stats[REPLAY_WALL_GE_RESTORE], wall_profile, phase_started);
        phase_started = wall_phase_begin(wall_profile);
        for (uint32_t list = 0; list < fixture.list_count; list++) {
            if (ge_run_list(fixture.list_addrs[list], 0) != 0) {
                fprintf(stderr, "ge_replay: list %u did not terminate at iteration %lu\n", list, i);
                return 3;
            }
        }
        wall_phase_end(&wall_stats[REPLAY_WALL_GE_LISTS], wall_profile, phase_started);
        if (wall_profile) {
            uint64_t frame_ns = SDL_GetTicksNS() - frame_started;
            uint64_t assigned_ns = 0;
            for (unsigned phase = REPLAY_WALL_RESET; phase <= REPLAY_WALL_GE_LISTS; phase++)
                assigned_ns += wall_stats[phase].ns;
            uint64_t frame_assigned_ns = assigned_ns - prior_assigned_ns;
            prior_assigned_ns = assigned_ns;
            wall_stats[REPLAY_WALL_LOOP_OTHER].calls++;
            wall_stats[REPLAY_WALL_LOOP_OTHER].ns +=
                frame_ns > frame_assigned_ns ? frame_ns - frame_assigned_ns : 0;
        }
    }
    uint64_t materialize_started = wall_phase_begin(wall_profile);
    if (vulkan && !gegpu_capture_materialize()) return 3;
    wall_phase_end(&wall_stats[REPLAY_WALL_MATERIALIZE], wall_profile, materialize_started);
    uint64_t elapsed = SDL_GetTicksNS() - started;
    if (!write_ppm(output, ge_state_ptr())) {
        fprintf(stderr, "ge_replay: could not write %s\n", output);
        return 3;
    }
    GeGpuReplayStats stats;
    memset(&stats, 0, sizeof(stats));
    if (vulkan) gegpu_replay_stats_get(&stats);
    GeGpuCpuProfileStats cpu_stats;
    memset(&cpu_stats, 0, sizeof(cpu_stats));
    if (vulkan) gegpu_cpu_profile_stats_get(&cpu_stats);
    GeCpuProfileStats ge_cpu_stats;
    memset(&ge_cpu_stats, 0, sizeof(ge_cpu_stats));
    ge_cpu_profile_get(&ge_cpu_stats);
    /* An enabled GE CPU profile is checked against the relations its producers establish.
     * A violation is printed with the numbers and fails the run (exit 4), and
     * tools/ge_replay_metrics.py refuses to parse a profile reported as FAIL. */
    int ge_cpu_validated = ge_cpu_stats.enabled || ge_cpu_stats.primitive_profile_enabled;
    char ge_cpu_detail[256] = "";
    uint32_t ge_cpu_failed = ge_cpu_validated
        ? ge_cpu_profile_validate(&ge_cpu_stats, ge_cpu_detail, sizeof ge_cpu_detail) : 0u;
    static const char *boundary_names[GEGPU_BOUNDARY_COUNT] = {
        "render_snapshot", "texture_upload", "target_upload", "depth_upload",
        "readback", "present", "lifetime", "other",
    };
    printf("GE_REPLAY backend=%s repeats=%lu wall_ms=%.3f ms_per_frame=%.6f frame=%u lists=%u pages=%u "
           "fbp=0x%08x fbw=%u fbfmt=%u "
           "queue_submits=%llu render_submits=%llu render_waits=%llu render_wait_ms=%.3f "
           "snapshot_requests=%llu snapshot_copies=%llu snapshot_submits=%llu "
           "snapshot_waits=%llu snapshot_wait_ms=%.3f mixed_submits=%llu mixed_waits=%llu mixed_wait_ms=%.3f "
           "batches=%llu draws=%llu upload_ring_reservations=%llu upload_ring_bytes=%llu "
           "upload_ring_wraps=%llu upload_ring_fallbacks=%llu upload_ring_high_water=%llu output=%s\n",
           backend, repeat, (double)elapsed / 1000000.0,
           (double)elapsed / 1000000.0 / (double)repeat,
           fixture.frame, fixture.list_count, fixture.page_count,
           ge_state_ptr()->fbp, ge_state_ptr()->fbw, ge_state_ptr()->fbfmt,
           stats.queue_submits, stats.render_submits, stats.render_waits,
           (double)stats.render_wait_ns / 1000000.0,
           stats.snapshot_requests, stats.snapshot_copies, stats.snapshot_submits,
           stats.snapshot_waits, (double)stats.snapshot_wait_ns / 1000000.0,
           stats.mixed_submits,
           stats.mixed_waits, (double)stats.mixed_wait_ns / 1000000.0,
           stats.batches, stats.draws,
           stats.upload_ring_reservations, stats.upload_ring_bytes,
           stats.upload_ring_wraps, stats.upload_ring_fallbacks,
           stats.upload_ring_high_water, output);
    fflush(stdout);
    if (vulkan) {
        for (unsigned i = 0; i < GEGPU_BOUNDARY_COUNT; i++) {
            const GeGpuReplayBoundaryStats *b = &stats.boundary[i];
            printf("GE_REPLAY_BOUNDARY reason=%s submits=%llu submit_ms=%.3f waits=%llu wait_ms=%.3f total_ms=%.3f\n",
                   boundary_names[i], b->submits, (double)b->submit_ns / 1000000.0,
                   b->waits, (double)b->wait_ns / 1000000.0,
                   (double)(b->submit_ns + b->wait_ns) / 1000000.0);
        }
        fflush(stdout);
    }
    if (vulkan && cpu_stats.enabled) {
        static const char *phase_names[GEGPU_CPU_PHASE_COUNT] = {
            "state_prep", "state_key", "pipeline_lookup", "pipeline_create",
            "descriptor_alloc", "descriptor_update", "bind_record", "object_lookup",
            "texture_decode", "texture_shadow", "vertex_prep", "snapshot_target", "snapshot_decision",
            "snapshot_region", "snapshot_metadata", "command_record", "memcpy",
            "heap",
        };
        for (unsigned i = 0; i < GEGPU_CPU_PHASE_COUNT; i++) {
            const GeGpuCpuPhaseStats *p = &cpu_stats.phase[i];
            printf("GE_REPLAY_CPU phase=%s calls=%llu ns=%llu ms=%.6f\n",
                   phase_names[i], p->calls, p->ns, (double)p->ns / 1000000.0);
        }
        printf("GE_REPLAY_CPU_COUNTS state_key_builds=%llu state_cache_hits=%llu state_cache_misses=%llu "
               "pipeline_hits=%llu pipeline_misses=%llu pipeline_creations=%llu "
               "descriptor_allocations=%llu descriptor_updates=%llu pipeline_binds=%llu "
               "pipeline_bind_redundant=%llu descriptor_binds=%llu descriptor_bind_redundant=%llu "
               "texture_hits=%llu texture_misses=%llu texture_shadow_checks=%llu "
               "texture_shadow_hits=%llu texture_shadow_bytes=%llu "
               "snapshot_requests=%llu snapshot_copies=%llu "
               "vertex_bytes=%llu memcpy_bytes=%llu "
               "target_calls=%llu target_fast_hits=%llu target_acquires=%llu "
               "ensure_room_calls=%llu ensure_room_flushes=%llu "
               "append_calls=%llu append_compare_calls=%llu append_merges=%llu\n",
               cpu_stats.state_key_builds, cpu_stats.state_cache_hits, cpu_stats.state_cache_misses,
               cpu_stats.pipeline_hits, cpu_stats.pipeline_misses, cpu_stats.pipeline_creations,
               cpu_stats.descriptor_allocations, cpu_stats.descriptor_updates,
               cpu_stats.pipeline_binds, cpu_stats.pipeline_bind_redundant,
               cpu_stats.descriptor_binds, cpu_stats.descriptor_bind_redundant,
               cpu_stats.texture_hits, cpu_stats.texture_misses,
               cpu_stats.texture_shadow_checks, cpu_stats.texture_shadow_hits,
               cpu_stats.texture_shadow_bytes,
               cpu_stats.snapshot_requests, cpu_stats.snapshot_copies,
               cpu_stats.vertex_bytes, cpu_stats.memcpy_bytes,
               cpu_stats.target_calls, cpu_stats.target_fast_hits, cpu_stats.target_acquires,
               cpu_stats.ensure_room_calls, cpu_stats.ensure_room_flushes,
               cpu_stats.append_calls, cpu_stats.append_compare_calls,
               cpu_stats.append_merges);
        for (unsigned i = 0; i < GEGPU_CPU_PHASE_COUNT; i++) {
            const GeGpuCpuPhaseStats *p = &cpu_stats.hook_phase[i];
            printf("GE_REPLAY_HOOK_CPU phase=%s calls=%llu ns=%llu ms=%.6f\n",
                   phase_names[i], p->calls, p->ns, (double)p->ns / 1000000.0);
        }
        printf("GE_REPLAY_HOOK_COUNTS calls=%llu submit_ns=%llu wait_ns=%llu\n",
               cpu_stats.hook_calls, cpu_stats.hook_submit_ns, cpu_stats.hook_wait_ns);
        fflush(stdout);
    }
    if (ge_cpu_validated) {
        printf("GE_REPLAY_GE_CPU_VALIDATION status=%s failed=", ge_cpu_failed ? "FAIL" : "PASS");
        if (!ge_cpu_failed) fputs("none", stdout);
        for (unsigned i = 0, n = 0; i < GE_PROFILE_CHECK_COUNT; i++)
            if (ge_cpu_failed & GE_PROFILE_CHECK_MASK(i))
                printf("%s%s", n++ ? "," : "", ge_profile_check_name((GeProfileCheck)i));
        putchar('\n');
        fflush(stdout);
        if (ge_cpu_failed)
            fprintf(stderr, "ge_replay: GE CPU profile failed validation: %s\n", ge_cpu_detail);
    }
    if (ge_cpu_stats.enabled) {
        static const char *phase_names[GE_CPU_PHASE_COUNT] = {
            "list_total", "command_dispatch", "primitive", "gpu_hook", "block_transfer", "clut_load", "flush",
        };
        for (unsigned i = 0; i < GE_CPU_PHASE_COUNT; i++) {
            const GeCpuPhaseStats *p = &ge_cpu_stats.phase[i];
            printf("GE_REPLAY_GE_CPU phase=%s calls=%llu ns=%llu ms=%.6f\n",
                   phase_names[i], (unsigned long long)p->calls,
                   (unsigned long long)p->ns, (double)p->ns / 1000000.0);
        }
        printf("GE_REPLAY_GE_CPU_COUNTS commands=%llu primitive_commands=%llu primitive_vertices=%llu "
               "vertex_decode_uses=%llu strip_vertices_reused=%llu "
               "prim_type0=%llu prim_type1=%llu prim_type2=%llu "
               "prim_type3=%llu prim_type4=%llu prim_type5=%llu prim_type6=%llu prim_type7=%llu "
               "block_transfers=%llu clut_loads=%llu flushes=%llu\n",
               (unsigned long long)ge_cpu_stats.commands,
               (unsigned long long)ge_cpu_stats.primitive_commands,
               (unsigned long long)ge_cpu_stats.primitive_vertices,
               (unsigned long long)ge_cpu_stats.vertex_decode_uses,
               (unsigned long long)ge_cpu_stats.strip_vertices_reused,
               (unsigned long long)ge_cpu_stats.primitive_type[0],
               (unsigned long long)ge_cpu_stats.primitive_type[1],
               (unsigned long long)ge_cpu_stats.primitive_type[2],
               (unsigned long long)ge_cpu_stats.primitive_type[3],
               (unsigned long long)ge_cpu_stats.primitive_type[4],
               (unsigned long long)ge_cpu_stats.primitive_type[5],
               (unsigned long long)ge_cpu_stats.primitive_type[6],
               (unsigned long long)ge_cpu_stats.primitive_type[7],
               (unsigned long long)ge_cpu_stats.block_transfers,
               (unsigned long long)ge_cpu_stats.clut_loads,
               (unsigned long long)ge_cpu_stats.flushes);
        fflush(stdout);
    }
    if (ge_cpu_stats.primitive_profile_enabled) {
        static const char *phase_names[GE_PRIM_PROFILE_PHASE_COUNT] = {
            "vertex_fetch_decode", "transform", "lighting",
            "clipping_acceptance", "primitive_assembly",
        };
        printf("GE_REPLAY_GE_PRIM_PROFILE_CONFIG stride=%u timer_pair_ns=%llu\n",
               ge_cpu_stats.primitive_profile_stride,
               (unsigned long long)ge_cpu_stats.primitive_profile_timer_pair_ns);
        for (unsigned i = 0; i < GE_PRIM_PROFILE_PHASE_COUNT; i++) {
            const GeCpuPhaseStats *p = &ge_cpu_stats.primitive_profile_phase[i];
            uint64_t eligible = ge_cpu_stats.primitive_profile_eligible[i];
            uint64_t estimate = primitive_profile_estimate_ns(p->ns, p->calls, eligible);
            printf("GE_REPLAY_GE_PRIM_PROFILE phase=%s calls=%llu ns=%llu ms=%.6f "
                   "eligible=%llu estimated_ns=%llu estimated_ms=%.6f\n",
                   phase_names[i], (unsigned long long)p->calls,
                   (unsigned long long)p->ns, (double)p->ns / 1000000.0,
                   (unsigned long long)eligible, (unsigned long long)estimate,
                   (double)estimate / 1000000.0);
        }
        if (ge_cpu_stats.primitive_profile_calibration_enabled) {
            const GeCpuPhaseStats *control = &ge_cpu_stats.primitive_profile_empty_control;
            const GeCpuPhaseStats *total = &ge_cpu_stats.primitive_profile_sampled_total;
            uint64_t total_eligible = ge_cpu_stats.primitive_profile_triangle_candidates;
            uint64_t total_estimate = primitive_profile_estimate_ns(total->ns, total->calls,
                                                                    total_eligible);
            uint64_t total_adjusted = primitive_profile_adjusted_estimate_ns(
                total->ns, total->calls, total_eligible);
            printf("GE_REPLAY_GE_PRIM_PROFILE_CALIBRATION enabled=1 adjustment=none\n");
            printf("GE_REPLAY_GE_PRIM_PROFILE_CONTROL calls=%llu ns=%llu ms=%.6f per_call_ns=%llu\n",
                   (unsigned long long)control->calls, (unsigned long long)control->ns,
                   (double)control->ns / 1000000.0,
                   (unsigned long long)(control->calls ? control->ns / control->calls : 0));
            printf("GE_REPLAY_GE_PRIM_PROFILE_TOTAL calls=%llu ns=%llu ms=%.6f "
                   "eligible=%llu estimated_ns=%llu estimated_ms=%.6f\n",
                   (unsigned long long)total->calls, (unsigned long long)total->ns,
                   (double)total->ns / 1000000.0, (unsigned long long)total_eligible,
                   (unsigned long long)total_estimate, (double)total_estimate / 1000000.0);
            printf("GE_REPLAY_GE_PRIM_PROFILE_TOTAL_ADJUSTED estimated_ns=%llu estimated_ms=%.6f\n",
                   (unsigned long long)total_adjusted, (double)total_adjusted / 1000000.0);
            for (unsigned i = 0; i < GE_PRIM_PROFILE_PHASE_COUNT; i++) {
                const GeCpuPhaseStats *p = &ge_cpu_stats.primitive_profile_phase[i];
                uint64_t eligible = ge_cpu_stats.primitive_profile_eligible[i];
                uint64_t adjusted = primitive_profile_adjusted_estimate_ns(
                    p->ns, p->calls, eligible);
                printf("GE_REPLAY_GE_PRIM_PROFILE_ADJUSTED phase=%s estimated_ns=%llu estimated_ms=%.6f\n",
                       phase_names[i], (unsigned long long)adjusted,
                       (double)adjusted / 1000000.0);
            }
        }
        printf("GE_REPLAY_GE_PRIM_PROFILE_COUNTS vertices=%llu transform_vertices=%llu "
               "triangle_candidates=%llu\n",
               (unsigned long long)ge_cpu_stats.primitive_profile_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_triangle_candidates);
        uint64_t primitive_commands = 0, submitted_primitives = 0;
        for (unsigned i = 0; i < 8; i++) {
            primitive_commands += ge_cpu_stats.primitive_profile_commands[i];
            submitted_primitives += ge_cpu_stats.primitive_profile_submitted[i];
        }
        printf("GE_REPLAY_GE_PRIM_PROFILE_POPULATION commands=%llu submitted=%llu "
               "vertex_references=%llu triangle_vertex_references=%llu "
               "non_triangle_vertex_references=%llu vertex_uses=%llu "
               "triangle_vertex_uses=%llu non_triangle_vertex_uses=%llu "
               "through_vertex_uses=%llu transform_vertex_uses=%llu "
               "actual_decoded_vertices=%llu actual_transformed_vertices=%llu "
               "actual_through_vertices=%llu strip_cache_commands=%llu strip_cache_hits=%llu "
               "through_triangle_candidates=%llu transform_triangle_candidates=%llu "
               "transform_triangles_drawn=%llu transform_triangles_clipped=%llu "
               "transform_triangles_rejected=%llu non_triangle_primitives=%llu "
               "vertex_rejects=%llu patch_commands=%llu patch_control_vertices=%llu\n",
               (unsigned long long)primitive_commands,
               (unsigned long long)submitted_primitives,
               (unsigned long long)ge_cpu_stats.primitive_profile_vertex_references,
               (unsigned long long)ge_cpu_stats.primitive_profile_triangle_vertex_references,
               (unsigned long long)ge_cpu_stats.primitive_profile_non_triangle_vertex_references,
               (unsigned long long)ge_cpu_stats.primitive_profile_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_triangle_vertex_uses,
               (unsigned long long)ge_cpu_stats.primitive_profile_non_triangle_vertex_uses,
               (unsigned long long)ge_cpu_stats.primitive_profile_through_vertex_uses,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_vertex_uses,
               (unsigned long long)ge_cpu_stats.primitive_profile_actual_decoded_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_actual_transformed_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_actual_through_vertices,
               (unsigned long long)ge_cpu_stats.primitive_profile_strip_cache_commands,
               (unsigned long long)ge_cpu_stats.primitive_profile_strip_cache_hits,
               (unsigned long long)ge_cpu_stats.primitive_profile_through_triangle_candidates,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_triangle_candidates,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_triangles_drawn,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_triangles_clipped,
               (unsigned long long)ge_cpu_stats.primitive_profile_transform_triangles_rejected,
               (unsigned long long)ge_cpu_stats.primitive_profile_non_triangle_primitives,
               (unsigned long long)ge_cpu_stats.primitive_profile_vertex_rejects,
               (unsigned long long)ge_cpu_stats.primitive_profile_patch_commands,
               (unsigned long long)ge_cpu_stats.primitive_profile_patch_control_vertices);
        printf("GE_REPLAY_GE_PRIM_PROFILE_TYPES commands_type0=%llu commands_type1=%llu "
               "commands_type2=%llu commands_type3=%llu commands_type4=%llu "
               "commands_type5=%llu commands_type6=%llu commands_type7=%llu "
               "submitted_type0=%llu submitted_type1=%llu submitted_type2=%llu "
               "submitted_type3=%llu submitted_type4=%llu submitted_type5=%llu "
               "submitted_type6=%llu submitted_type7=%llu\n",
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[0],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[1],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[2],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[3],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[4],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[5],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[6],
               (unsigned long long)ge_cpu_stats.primitive_profile_commands[7],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[0],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[1],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[2],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[3],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[4],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[5],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[6],
               (unsigned long long)ge_cpu_stats.primitive_profile_submitted[7]);
        fflush(stdout);
    }
    if (vulkan && cpu_stats.enabled && ge_cpu_stats.enabled) {
        uint64_t renderer_ns = 0;
        for (unsigned i = 0; i < GEGPU_CPU_PHASE_COUNT; i++)
            renderer_ns += cpu_stats.hook_phase[i].ns;
        uint64_t list_ns = ge_cpu_stats.phase[GE_CPU_LIST_TOTAL].ns;
        uint64_t command_ns = ge_cpu_stats.phase[GE_CPU_COMMAND_DISPATCH].ns;
        uint64_t primitive_ns = ge_cpu_stats.phase[GE_CPU_PRIMITIVE].ns;
        uint64_t gpu_hook_ns = ge_cpu_stats.phase[GE_CPU_GPU_HOOK].ns;
        uint64_t block_ns = ge_cpu_stats.phase[GE_CPU_BLOCK_TRANSFER].ns;
        uint64_t clut_ns = ge_cpu_stats.phase[GE_CPU_CLUT_LOAD].ns;
        uint64_t flush_ns = ge_cpu_stats.phase[GE_CPU_FLUSH].ns;
        uint64_t top_assigned = command_ns + primitive_ns + block_ns + clut_ns + flush_ns;
        uint64_t hook_assigned = renderer_ns + cpu_stats.hook_submit_ns + cpu_stats.hook_wait_ns;
        printf("GE_REPLAY_HIERARCHY list_ns=%llu command_ns=%llu primitive_ns=%llu "
               "primitive_frontend_ns=%llu gpu_hook_ns=%llu block_ns=%llu clut_ns=%llu "
               "flush_ns=%llu list_residual_ns=%llu hook_renderer_ns=%llu hook_submit_ns=%llu "
               "hook_wait_ns=%llu hook_residual_ns=%llu\n",
               (unsigned long long)list_ns, (unsigned long long)command_ns,
               (unsigned long long)primitive_ns,
               (unsigned long long)(primitive_ns > gpu_hook_ns ? primitive_ns - gpu_hook_ns : 0),
               (unsigned long long)gpu_hook_ns, (unsigned long long)block_ns,
               (unsigned long long)clut_ns, (unsigned long long)flush_ns,
               (unsigned long long)(list_ns > top_assigned ? list_ns - top_assigned : 0),
               (unsigned long long)renderer_ns,
               (unsigned long long)cpu_stats.hook_submit_ns,
               (unsigned long long)cpu_stats.hook_wait_ns,
               (unsigned long long)(gpu_hook_ns > hook_assigned ? gpu_hook_ns - hook_assigned : 0));
        fflush(stdout);
    }
    if (wall_profile) {
        static const char *phase_names[REPLAY_WALL_PHASE_COUNT] = {
            "replay_reset", "fixture_apply", "ge_restore", "ge_lists", "loop_other", "materialize",
        };
        static const char *class_names[REPLAY_WALL_PHASE_COUNT] = {
            "HARNESS-ONLY", "HARNESS-ONLY", "HARNESS-ONLY", "PRODUCTION-RELEVANT",
            "HARNESS-ONLY", "PRODUCTION-RELEVANT",
        };
        for (unsigned i = 0; i < REPLAY_WALL_PHASE_COUNT; i++) {
            const ReplayWallPhaseStats *p = &wall_stats[i];
            printf("GE_REPLAY_WALL_PHASE phase=%s classification=%s calls=%llu ns=%llu ms=%.6f\n",
                   phase_names[i], class_names[i], (unsigned long long)p->calls,
                   (unsigned long long)p->ns, (double)p->ns / 1000000.0);
        }
        fflush(stdout);
    }

    if (vulkan) { gegpu_shutdown(); sdl3vk_shutdown(); }
    ge_capture_free(&fixture);
    free(arena);
    return ge_cpu_failed ? 4 : 0;
}
