// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-11.
// See NOTICE.md for upstream lineage and modification provenance.
//
/*
 * AI disclosure: original Vulkan backend (NOT a port of PPSSPP's GPU), written with
 * substantial assistance from an LLM (Anthropic Claude). It reproduces PSP/GE pixel rules
 * derived from PPSSPP's software renderer (via ge.c), so it is GPLv2+. See NOTICE.md.
 *
 * Architecture: ge.c keeps doing vertex decode + T&L + clipping + primitive acceptance
 * (the parts validated against PPSSPP), then hands SCREEN-SPACE primitives to this
 * backend through the GeGpuHooks seam. Every primitive renders on the GPU — there is no
 * software-rasterizer fallback; unmappable blend modes are approximated (see
 * build_state). GE framebuffers are PERSISTENT GPU images (color RGBA8 + shared D16
 * depth keyed by zbp): no guest-VRAM upload/readback per flush. Guest VRAM is read only
 * when a target is first created (or invalidated by a CPU write: movie decode, DMA),
 * and written only on the rare paths that genuinely consume it (CLUT load from VRAM,
 * target eviction). Presentation blits the GPU image directly to the swapchain.
 *
 * Batches accumulate across GE lists and are submitted only when something consumes the
 * target: a retarget, a present, self-sampling (render-to-texture feedback), buffer
 * exhaustion. A submit is GPU-only (record + submit + fence); there is no pixel
 * conversion on the CPU in the steady state.
 *
 * Faithfulness notes (vs ge.c, the reference):
 *  - Coverage: x/y pre-snapped to the PSP 28.4 grid; Vulkan rasterizes at pixel centers
 *    with a top-left fill rule, matching raster_tri's integer edges.
 *  - Interpolation: all varyings noperspective; u*rw, v*rw, rw interpolate affinely and
 *    divide per pixel, exactly like the software inner loop.
 *  - Culling: draw_prim already reorders vertices for cull mode/strip parity; raster_tri
 *    keeps positive-cross (clockwise in Vulkan's framebuffer convention), so front face
 *    is CLOCKWISE with back-face culling.
 *  - Textures decode through ge.c's own sampler (ge_decode_tex_rgba); CLUT/swizzle is
 *    reference-correct by construction. Render-to-texture binds the target image
 *    directly when the address/stride match (a snapshot copy for feedback loops).
 *  - Mip levels (#1373): when ge_tex_mip_active() turns them on, a cached texture holds
 *    levels 0..TEXMODE's highest level as one Vulkan mip chain, each level decoded by ge.c
 *    (ge_decode_tex_level_rgba). TEXLEVEL constant mode (and the unused mode 3) is one level
 *    of detail per draw, the level and blend ge_tex_level_pick takes from the bias (minLod =
 *    maxLod); TEXFILTER bit 1 is the mipmap mode. The automatic and slope modes' level of
 *    detail is per fragment in the PSP's own arithmetic (ge.c tex_detail16, measured as
 *    PSP-GE-MIP-001), which the sampler's exact log2 does not reproduce, so psp.frag
 *    computes it (F_LODFRAG) and samples with an explicit level of detail. A level list a
 *    halving chain cannot hold takes the software rasterizer (tex_mip_chain_supported).
 *    Per-level CLUTs are not modelled.
 *  - 16-bit precision (5650/5551/4444 framebuffers): the fragment shader snaps its
 *    output to the native lattice (pack-by-truncation + expand-by-bit-replication,
 *    identical to ge.c pack_fb/unpack_color) and applies the GE's 4x4 ordered dither,
 *    so the RGBA8 target image round-trips guest VRAM bit-exactly — no color or
 *    depth drift across consecutive frames.
 *  - Blending: simple states map to Vulkan fixed function (min/max are exact because
 *    both the PSP and Vulkan ignore the factors). Everything fixed function cannot
 *    express — every doubled alpha factor, absdiff, equations 6 and 7, two distinct FIX
 *    constants, and any blend onto a 16-bit target
 *    (the hardware quantizes/dithers the blend RESULT on store) — runs as SHADER
 *    blending: the fragment shader reads a snapshot of the destination and evaluates
 *    ge.c's integer blend_chan()/factor_component() formulas verbatim. The snapshot is
 *    refreshed per state build (pending batches submit first), so the one remaining
 *    approximation is overlapping primitives INSIDE a single batch reading the
 *    pre-batch destination — the standard shader-blend hazard, same as PPSSPP.
 *  - Stencil (the PSP keeps it in destination alpha): the fragment shader runs ge.c's
 *    stencil test and ops against the destination snapshot, with pipeline blending off.
 *    Where the depth test runs, its fail op and the stencil-fail op must be KEEP, so the
 *    pipeline depth test and a discard give ge.c's order exactly (stencil_gpu_supported);
 *    other states take the software rasterizer. A stencil state whose result depends on
 *    the stencil it overwrites (stencil_reads_own_writes) never shares a batch between
 *    primitives whose boxes meet, so each reads a snapshot holding the ones before it.
 *  - Approximated (no software fallback): partial-byte write masks (>= 0x80 disables
 *    the channel), lines/points (1px quads, not DDA).
 *  - Render scale (default 1): SR_RESOLUTION_SCALE carries the player's Settings >
 *    Internal Render Resolution choice (1..4); SR_GPU_SCALE overrides it as an
 *    explicit diagnostic (sr_parse_render_scale in ge_gpu.h). Targets/depth/snapshot images are
 *    allocated at scale x the 512x272 canvas and the viewport/scissor scale with them;
 *    vertices map through the same NDC transform, so higher scales are pure
 *    magnification of the PSP raster grid. Guest VRAM traffic stays at native
 *    resolution (readback samples the top-left subpixel of each guest pixel; upload
 *    replicates), and the ordered-dither pattern stays keyed to GUEST pixels.
 *    Bit-exact VRAM round-trips hold at scale 1 (readback(upload(x)) == x by
 *    construction); higher scales trade that for resolution, like any upscaler.
 */
#include "ge_gpu.h"
#include "sdl3vk.h"

#define _CRT_SECURE_NO_WARNINGS
#include "../recomp.h"
#include "../ge_shared.h"
#include "../perf.h"
#include "../ace.h"   /* Ace Mode: the 30 Hz detector counts draws per target (Ace on only) */

#include <vulkan/vulkan.h>
#include <SDL3/SDL_timer.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

#define FB_W 512            /* full PSP framebuffer stride (guest units) */
#define FB_H 272
#define DRAW_W 480          /* visible display width (presentation crop only) */
#define MAX_SCALE 4
static int s_scale = 1;     /* integer render scale (SR_GPU_SCALE), 1..MAX_SCALE */
#define SCL_W ((uint32_t)(FB_W * s_scale))   /* device-pixel canvas */
#define SCL_H ((uint32_t)(FB_H * s_scale))
#define MAX_VERTS  131072
#define MAX_BATCH  4096
#define PIPE_CACHE_INITIAL 192   /* growable: the state key space is not bounded by a constant */
#define MAX_TEX    1024
#define MAX_TGT    8
#define MAX_DEP    4
#define READBACK_FRAMES 3
#define SUBMIT_FRAMES 8
/* Recorded GE-ring ops (render batches, destination snapshot copies, ring uploads) that
 * share one vkQueueSubmit: a submission granularity, not an ordering boundary. All GE work
 * goes to one queue, and the GPU-side order between ops comes from what is recorded in the
 * stream (to_layout barriers, render-pass dependencies); a queue submission adds no
 * device-to-device memory dependency, so where a command buffer ends cannot have been
 * providing one. Nothing CPU-visible relies on the cap either: target and depth readback,
 * dirty-edge reads, block-transfer blits and the shared-staging upload fallback close the
 * open chain (cmd_begin) and wait on their own fence; capture materialization and every
 * destruction (texture eviction, target, depth image, framebuffer) drain all slots;
 * presentation closes the chain before its readback submit (target_prepare_present). The
 * per-slot vertex arena and upload ring flush on their own capacity checks and are
 * rewritten only after their slot's fence. The legacy cap of 8 cost a submit, and a fence
 * wait when the slot came round again, every eight ops: about 1,500 of each per second on
 * the windowed player route, 95-115 ms of every second in vulkan_submit + ge_wait (#827).
 * The cost of a larger cap is overlap: the GPU starts an op only once its command buffer is
 * submitted. SR_GPU_SUBMIT_BATCH=1..SUBMIT_BATCH_MAX overrides the limit (8 restores the
 * legacy schedule) and SR_GPU_SYNC_SUBMIT=1 forces one op per submit. The coherence
 * selftest's submit-batch fixture pins the frames against ge.c and the submit accounting. */
#define SUBMIT_BATCH_MAX 128
#define XFER_RING_DEFAULT_BYTES (8u << 20)
#define TEX_SHADOW_MAX_BYTES (64u << 20)
/* A slot can retain more than one recorded render before submission. Each render gets
 * a disjoint range; exhaustion submits early rather than overwriting vertices that an
 * earlier vkCmdDraw in the same command buffer still references. */
#define VERT_ARENA_VERTS (MAX_VERTS * 2u)
/* GPU transform and lighting (#318): staged vertex references and PRIM parameter records,
 * and their per-slot arenas, allocated the first time a PRIM arrives (SR_GPU_TNL is opt-in). */
#define MAX_TNL_VERTS  65536u
#define MAX_TNL_PARAMS 2048u
#define TNL_ARENA_VERTS (MAX_TNL_VERTS * 2u)
#define TNL_ARENA_PARAMS (MAX_TNL_PARAMS * 2u)

static const uint32_t k_vert_spv[] =
#include "psp_vert.inc"
;
static const uint32_t k_frag_spv[] =
#include "psp_frag.inc"
;
static const uint32_t k_tnl_vert_spv[] =
#include "psp_tnl_vert.inc"
;
/* psp_tnl.vert reads GeTnlParams as P_WORDS 32-bit words and GeTnlVtx as the vertex layout
 * pipe_create gives it. */
_Static_assert(sizeof(GeTnlParams) == 227u * 4u, "psp_tnl.vert P_WORDS");
_Static_assert(sizeof(GeTnlVtx) == 72u, "psp_tnl.vert vertex layout");

/* ---- captured vertex (must match psp.vert input layout) ------------------------------ */
typedef struct { float x, y, z, rw, u, v, fog; uint32_t rgba; } GpuVert;

/* fragment push constants (must match psp.frag PC block; 128 bytes = the guaranteed
 * VK minimum maxPushConstantsSize) */
typedef struct { int32_t cfg[4]; float texenv[4]; float fogcol[4]; float texsize[4];
                 int32_t dith[4]; int32_t bl[4]; float fixa[4]; float fixb[4]; } PushPC;
enum { F_TEX = 1, F_RGBA = 2, F_DBL = 4, F_FOG = 8, F_PERSP = 16, F_CLEAR = 32,
       F_NEAREST = 64, F_SA2X = 128, F_SA2XI = 256, F_DITHER = 512, F_SHBLEND = 4096,
       F_STENCIL = 8192, F_LODFRAG = 16384 };
#define F_FMT_SHIFT 10                     /* bits 10-11: framebuffer psm for quantization */

typedef struct {
    uint8_t blend_on, srcf, dstf, eq;      /* VkBlendFactor / VkBlendOp values */
    uint8_t cull;                          /* VkCullModeFlagBits */
    uint8_t ztest, zwrite, zfunc;          /* VkCompareOp */
    uint8_t cmask;                         /* VkColorComponentFlags */
    uint8_t tnl;                           /* 1: psp_tnl.vert over GeTnlVtx (#318) */
} PipeKey;

typedef struct {
    PipeKey key;
    int32_t sx, sy, sw, sh;                /* scissor rect */
    float   bconst[4];                     /* blend constants (FIX factors) */
    PushPC  pc;
    VkDescriptorSet dset;
    uint32_t first, count;
} Batch;

/* Everything besides level 0 that shapes a cached mip chain and its sampler (#1373). All
 * zero when mip levels are off, so a level-0-only entry compares equal to any other. */
typedef struct {
    uint32_t max_level;                    /* TEXMODE highest level; 0 = level 0 only */
    uint32_t mode;                         /* TEXLEVEL mode: 0 automatic, 2 slope, 1 or 3 constant */
    uint32_t linear;                       /* TEXFILTER bit 1: blend the two nearest levels */
    float bias;                            /* TEXLEVEL bias (0 in the per-fragment modes) */
    uint32_t addr[7], bufw[7];             /* levels 1..max_level (index = level - 1) */
    uint8_t wlog2[7], hlog2[7];
} TexMipKey;

typedef struct {
    uint64_t key, hash;                    /* state key + content hash; 0,0 = empty */
    VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp;
    VkDescriptorSet set;
    /* Ace Mode's texture filtering (ace.h): the same sampler with anisotropy, and its set,
     * made the first time a qualifying draw binds this entry. Never made in an authentic run. */
    VkSampler smp_af;
    VkDescriptorSet set_af;
    int w, h;
    uint32_t addr, bytes;                  /* guest source range for precise CPU-write invalidation */
    TexMipKey mip;                         /* compared exactly on lookup, with key */
    uint32_t mip_bytes[7];                 /* source range of each level 1..mip.max_level */
    uint64_t range_lo[8], range_hi[8];     /* canonical source ranges, level 0 first (#282) */
    int range_n;
    int content_valid;
    int pending;                           /* referenced by not-yet-submitted batches */
    uint8_t *shadow;
    uint32_t shadow_bytes;
    uint8_t shadow_clut[2048];
    uint32_t shadow_clut_fmt;
    int shadow_clut_valid;
    uint64_t lru;
} TexEnt;

typedef struct { PipeKey key; VkPipeline pipe; int used; } PipeEnt;

#define TEX_FILTER_VARIANTS 4u
#define FIXED_FILTER_DESCRIPTOR_SETS (TEX_FILTER_VARIANTS * (MAX_TGT + 1u) + 1u)

/* TFLT stores independent min and mag base filters in bits 0 and 8. */
static unsigned tex_filter_variant(uint32_t tflt) {
    return ((tflt & 1u) ? 2u : 0u) | ((tflt & 0x100u) ? 1u : 0u);
}

/* shared depth buffer, keyed by guest zbuf placement */
typedef struct {
    int used;
    uint32_t zba, zstride;                 /* key: zbp & 0x1FFFFF, stride */
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
    int cpu_dirty;                         /* software fallback changed s_zbuf */
    int guest_dirty;                        /* Vulkan depth is newer than guest VRAM */
} DepthEnt;

/* CPU writes into a resident target are held as up to this many byte spans and applied
 * together when the GE next uses the target; a write that would need one more span first
 * applies the held ones. */
#define TARGET_CPU_SPANS 32

/* persistent GE render target */
typedef struct {
    int used;
    uint32_t fba, stride, fmt;             /* key: framebuffer addr (masked), stride px, psm */
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
    VkDescriptorSet set[TEX_FILTER_VARIANTS]; /* sampled by TFLT min/mag pair for RTT + blit src */
    DepthEnt *dep;
    VkFramebuffer fb;                      /* for (this color, this->dep) */
    int gpu_valid;                         /* GPU contents are the truth (newer than VRAM) */
    uint64_t lru;
    uint64_t render_gen;                   /* bumped per submit that rendered into this */
    uint64_t clean_gen;                    /* render_gen when guest VRAM last matched */
    /* CPU writes to this target's VRAM not yet in the image: merged, disjoint, sorted
     * [lo, hi) VRAM-offset byte spans (target_note_cpu_write, target_sync_cpu_writes). */
    uint32_t cpu_lo[TARGET_CPU_SPANS], cpu_hi[TARGET_CPU_SPANS];
    int cpu_n;
} Target;

/* Presentation readbacks use their own command buffer, fence, and persistently mapped
 * buffer.  This lets the guest-VRAM conversion complete on a later emulator tick while
 * the swapchain consumes the target image immediately. */
typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint32_t *map;
    Target *target;
    VkImage image;
    uint64_t gen;
    uint32_t fba, stride, fmt;
    int pending;
    int commit;
} ReadbackSlot;

typedef struct {
    VkCommandBuffer cmd;
    VkFence fence;
    VkBuffer vbuf;
    VkDeviceMemory vbuf_mem;
    GpuVert *vmap;
    VkBuffer xfer;
    VkDeviceMemory xfer_mem;
    uint8_t *xfer_map;
    VkDeviceSize xfer_used;
    SrPerfGeReason reason;
    uint32_t reason_mask;
    uint32_t recorded_ops;
    uint32_t vused;
    /* GPU T&L arenas (#318): vertex references, parameter records, and the set binding them. */
    VkBuffer tbuf, pbuf;
    VkDeviceMemory tbuf_mem, pbuf_mem;
    GeTnlVtx *tmap;
    GeTnlParams *pmap;
    VkDescriptorSet tset;
    uint32_t tused, pused;
    int in_flight;
} SubmitSlot;

/* ---- backend state -------------------------------------------------------------------- */
static int s_ready = 0;
/* Ace Mode's anisotropy level (ace.h), read once at init: 0 in an authentic run, and when the
 * device gives none. Only then is any anisotropic sampler made. */
static int s_ace_af = 0;
static uint64_t s_cnt_af_samplers = 0;
static uint32_t s_tex_pool_sets = 0;
static GeState  *s_ge;
static uint16_t *s_zbuf;

static VkPhysicalDevice s_pdev;
static VkDevice   s_dev;
static VkQueue    s_queue;
static VkCommandPool s_pool;
static VkCommandBuffer s_cmd;
static SubmitSlot s_submit[SUBMIT_FRAMES];
static SubmitSlot *s_cmd_slot;
static uint32_t s_submit_cursor;
static int s_async_submit;
static uint32_t s_submit_batch_ops;
static VkDeviceSize s_xfer_ring_bytes;
static VkDeviceSize s_xfer_align;
static ReadbackSlot s_readback[READBACK_FRAMES];
static uint32_t s_readback_cursor;
/* Background presentation readbacks have no synchronous caller to return an error to.
 * Keep failures sticky so the explicit snapshot boundary can refuse stale guest VRAM. */
static unsigned long long s_readback_commit_failures;

static VkRenderPass s_rp;

static VkBuffer s_xfer;                    /* pixel transfer staging (at least 1MB) */
static VkDeviceMemory s_xfer_m;
static void    *s_xfer_map;
static GpuVert s_vcpu[MAX_VERTS];

static VkDescriptorSetLayout s_dlayout;
static VkDescriptorPool s_dpool_tex, s_dpool_fix;
static VkPipelineLayout s_playout;
static VkShaderModule s_vs, s_fs;
static VkShaderModule s_vs_tnl;
static VkDescriptorSetLayout s_tnl_dlayout;   /* set 1: the slot's GeTnlParams arena */
static VkDescriptorPool s_tnl_dpool;
static int s_tnl_ready;                       /* arenas and sets allocated */
static GeTnlVtx *s_tnl_vcpu;                  /* staged references, three per triangle */
static GeTnlParams *s_tnl_pcpu;               /* staged parameter records */
static uint32_t s_ntnl, s_ntnlp;
/* Screen box of each staged T&L PRIM (from ge.c's clip-space bound), for batch_rect. */
typedef struct { uint32_t first, end; float x0, y0, x1, y1; } TnlBox;
static TnlBox *s_tnl_box;
static unsigned long s_cnt_tnl_prims, s_cnt_tnl_tris;

static TexEnt  s_tex[MAX_TEX];
static int     s_tex_n = 0;

/* CPU-store page index (#282). hook_vram_dirty runs for every guest store into VRAM, and
 * comparing each store with every cache entry was the largest single host cost of titles whose
 * CPU writes VRAM. Two facts about 4 KiB pages of the canonical physical space SR_PHYS covers
 * let a store skip that scan:
 *   clean  no entry's source range (any level) touches the page. Entry ranges never change
 *          after tex_get creates the entry and eviction only removes ranges, so the bit stays
 *          true until a new entry covers the page; creating one clears exactly its pages.
 *   quiet  every entry touching the page is already invalid. Recorded with s_tex_valid_gen,
 *          which moves whenever an entry is created or becomes valid again, so a quiet page
 *          stays quiet only while no entry has been (re)validated anywhere.
 * A store wholly inside clean pages changes nothing. A store wholly inside quiet pages could
 * only re-invalidate invalid entries; it drops the state template, which a store overlapping
 * an invalid entry also does, so every later draw sees the same textures. */
#define TEX_CLEAN_PAGE_SHIFT 12u
#define TEX_CLEAN_PAGES (0x20000000u >> TEX_CLEAN_PAGE_SHIFT)
static uint64_t s_tex_clean[TEX_CLEAN_PAGES / 64u];
static uint32_t s_tex_quiet[TEX_CLEAN_PAGES];    /* s_tex_valid_gen when found quiet; 0 = unknown */
static uint32_t s_tex_valid_gen = 1;

/* CPU HLE calls may use any PSP address alias (KSEG mirrors for RAM or the mirrored
 * 0x04xxxxxx VRAM aperture); cache ranges are compared in one physical namespace. */
static uint32_t tex_canonical(uint32_t addr) {
    return sr_vram_cpu_aperture(addr) ? sr_vram_cpu_address(addr) : SR_PHYS(addr);
}

/* Cache the canonical source range of level 0, then of each level 1..mip.max_level (#1373). */
static void tex_ent_set_ranges(TexEnt *e) {
    int n = 0;
    e->range_lo[n] = tex_canonical(e->addr);
    e->range_hi[n] = e->range_lo[n] + e->bytes;
    n++;
    for (uint32_t l = 0; l < e->mip.max_level && l < 7u; l++, n++) {
        e->range_lo[n] = tex_canonical(e->mip.addr[l]);
        e->range_hi[n] = e->range_lo[n] + e->mip_bytes[l];
    }
    e->range_n = n;
}

static int tex_ent_overlaps(const TexEnt *e, uint64_t a0, uint64_t a1) {
    for (int i = 0; i < e->range_n; i++)
        if (a0 < e->range_hi[i] && e->range_lo[i] < a1) return 1;
    return 0;
}

/* Whether every page of [p0, p1] is known to hold no cached texture. */
static int tex_clean_pages(uint64_t p0, uint64_t p1) {
    for (uint64_t p = p0; p <= p1; p++) {
        uint64_t word = s_tex_clean[p >> 6];
        if ((p & 63u) == 0 && p + 63u <= p1) {
            if (word != UINT64_MAX) return 0;
            p += 63u;
            continue;
        }
        if (!(word & (UINT64_C(1) << (p & 63u)))) return 0;
    }
    return 1;
}

static void tex_clean_set(uint64_t p0, uint64_t p1, int clean) {
    for (uint64_t p = p0; p <= p1 && p < TEX_CLEAN_PAGES; p++) {
        if (clean) s_tex_clean[p >> 6] |= UINT64_C(1) << (p & 63u);
        else s_tex_clean[p >> 6] &= ~(UINT64_C(1) << (p & 63u));
    }
}

static int tex_quiet_pages(uint64_t p0, uint64_t p1) {
    for (uint64_t p = p0; p <= p1; p++)
        if (s_tex_quiet[p] != s_tex_valid_gen) return 0;
    return 1;
}

/* An entry became valid (created, revalidated or re-uploaded): no page is known quiet. */
static void tex_note_valid(void) {
    if (++s_tex_valid_gen == 0u) {          /* wrapped: no stale mark may match again */
        memset(s_tex_quiet, 0, sizeof(s_tex_quiet));
        s_tex_valid_gen = 1u;
    }
}

/* A new entry: its pages may hold a cached texture from now on. */
static void tex_clean_forget(const TexEnt *e) {
    for (int i = 0; i < e->range_n; i++)
        if (e->range_hi[i] > e->range_lo[i])
            tex_clean_set(e->range_lo[i] >> TEX_CLEAN_PAGE_SHIFT,
                          (e->range_hi[i] - 1u) >> TEX_CLEAN_PAGE_SHIFT, 0);
}
static VkImage s_white; static VkDeviceMemory s_white_mem;
static VkImageView s_white_view; static VkSampler s_white_smp;
static VkDescriptorSet s_white_set;

/* snapshot copy of a target for self-sampling (feedback) draws */
static VkImage s_snapimg; static VkDeviceMemory s_snapimg_mem;
static VkImageView s_snap_view; static VkImageLayout s_snap_layout;
static VkSampler s_smp[TEX_FILTER_VARIANTS]; /* shared clamp samplers indexed by TFLT pair */
static VkDescriptorSet s_snap_set[TEX_FILTER_VARIANTS];
static Target *s_snap_src = NULL;          /* what the snapshot currently holds */
static uint64_t s_snap_srcgen = 0;

/* Deferred shader-blend snapshots. A shader-blend batch reads the destination snapshot only at
 * its own fragments (texelFetch(u_dst, gl_FragCoord.xy) in psp.frag), and every fragment a batch
 * writes or reads lies inside the box of its vertices clipped to its scissor (batch_rect).
 * s_snap_dirty is the union of the boxes of the render ops drawn into s_snap_src since its last
 * copy, valid while s_snap_src->render_gen == s_snap_dirty_gen: outside the box the snapshot
 * equals the target. A shader-blend batch whose box misses the box reads from the snapshot what
 * a fresh copy would hold, so submit_pending records a copy only when the boxes meet, when the
 * snapshot holds another target, or when the target changed by any other path (its render_gen
 * moved on without this bookkeeping). */
typedef struct { int x0, y0, x1, y1; } SnapRect;   /* guest pixels, half-open; x0 >= x1: empty */
static SnapRect s_snap_dirty;
static uint64_t s_snap_dirty_gen;
static int s_snap_tracked;                 /* s_snap_dirty describes s_snap_src */

static PipeEnt *s_pipes = NULL;
static int s_pipe_n = 0, s_pipe_cap = 0;

static Target   s_tgts[MAX_TGT];
static DepthEnt s_deps[MAX_DEP];
static Target  *s_cur = NULL;              /* target of the pending batches */
static uint64_t s_lru = 1;

static Batch    s_batch[MAX_BATCH];
static uint32_t s_nbatch = 0, s_nverts = 0;

static int s_log = 0;
static int s_stats = 0;
static int s_tex_shadow_enabled = 1;
static size_t s_tex_shadow_bytes;
static unsigned long s_cnt_submit = 0, s_cnt_tri = 0, s_cnt_spr = 0, s_cnt_line = 0;
static unsigned long s_cnt_nonfinite = 0;
static uint64_t s_cnt_batches = 0, s_cnt_verts = 0;
unsigned long s_cnt_present_gpu = 0;

/* Public read-only accessor: number of GE list submissions since process start.
 * Unused by the recomp/runtime; kept available for downstream tools/diffing. */
unsigned long sr_submit_count(void) { return s_cnt_submit; }
static unsigned long s_cnt_present_cpu = 0, s_cnt_rtt = 0;
static unsigned long s_cnt_readback = 0, s_cnt_upload = 0, s_cnt_dirty = 0;
static unsigned long s_cnt_snap = 0, s_cnt_texup = 0, s_cnt_xferblit = 0;
static uint64_t s_cnt_tex_invalidations = 0;
static uint64_t s_cnt_tex_clean_skips = 0;   /* CPU stores that skipped the texture scan (#282): */
static uint64_t s_cnt_tex_quiet_skips = 0;   /* clean pages, and quiet pages */
static uint64_t s_cnt_shadow_checks = 0, s_cnt_shadow_hits = 0, s_cnt_shadow_misses = 0;
static uint64_t s_cnt_shadow_bytes = 0, s_cnt_shadow_avoided = 0, s_cnt_shadow_required = 0;
static GeGpuReplayStats s_replay_stats;
static GeGpuCpuProfileStats s_cpu_profile_stats;
static int s_cpu_profile;
static int s_cpu_profile_hook_depth;

static uint32_t s_texscratch[512 * 512];
static uint32_t s_pxscratch[FB_W * FB_H];
/* One guest row at device width (each pixel repeated s_scale times), in cached memory. The
 * s_scale device rows of an upload are each copied from here. The upload ring is TRANSFER_SRC
 * memory without HOST_CACHED (gegpu_pick_host_mem_type), write-combined on common GPUs, where
 * every CPU read goes to memory: copying each replicated row from the first row of its cell
 * in the mapped buffer was about half of a 2x frame's CPU time in a measured title (#1296). */
static uint32_t s_pxwide[FB_W * MAX_SCALE];
static uint16_t s_zwide[FB_W * MAX_SCALE];

#define VKC(expr) do { VkResult vr_ = (expr); if (vr_ != VK_SUCCESS) { \
    fprintf(stderr, "gegpu: %s failed: %d\n", #expr, (int)vr_); return 0; } } while (0)

static inline uint64_t cpu_profile_now(void) {
    return s_cpu_profile ? SDL_GetTicksNS() : 0;
}

static inline void cpu_profile_add(GeGpuCpuPhase phase, uint64_t started) {
    if (!s_cpu_profile) return;
    uint64_t elapsed = SDL_GetTicksNS() - started;
    s_cpu_profile_stats.phase[phase].calls++;
    s_cpu_profile_stats.phase[phase].ns += elapsed;
    if (s_cpu_profile_hook_depth > 0) {
        s_cpu_profile_stats.hook_phase[phase].calls++;
        s_cpu_profile_stats.hook_phase[phase].ns += elapsed;
    }
}

static inline void cpu_profile_add_elapsed(GeGpuCpuPhase phase, uint64_t elapsed) {
    if (!s_cpu_profile) return;
    s_cpu_profile_stats.phase[phase].ns += elapsed;
    if (s_cpu_profile_hook_depth > 0)
        s_cpu_profile_stats.hook_phase[phase].ns += elapsed;
}

static inline void cpu_profile_hook_enter(void) {
    if (!s_cpu_profile) return;
    s_cpu_profile_hook_depth++;
    s_cpu_profile_stats.hook_calls++;
}

static inline int cpu_profile_hook_leave(int result) {
    if (s_cpu_profile) s_cpu_profile_hook_depth--;
    return result;
}

static inline void cpu_profile_vertex_done(uint64_t started, uint32_t vertices) {
    if (!s_cpu_profile) return;
    cpu_profile_add(GEGPU_CPU_VERTEX_PREP, started);
    s_cpu_profile_stats.vertex_bytes += (uint64_t)vertices * sizeof(GpuVert);
}

/* ---- small helpers -------------------------------------------------------------------- */

/* PSP VRAM is 2 MiB at 0x04000000, mirrored across the whole 0x04xxxxxx window.  The
 * 20-bit fold in vram_off() reproduces the hardware mirror — it must only ever be
 * applied to addresses is_vram() accepts.  GE color buffers can also legally live in
 * main RAM (render-to-RAM); those states are NOT representable as persistent GPU
 * targets here and are routed to the ge.c software path instead of being silently
 * folded into the VRAM window (which aliased them onto unrelated targets). */
static inline int is_vram(uint32_t addr) { return (addr & 0x0F000000u) == 0x04000000u; }
static inline uint32_t vram_off(uint32_t addr) { return addr & 0x001FFFFFu; }

static uint32_t find_mem(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s_pdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

/* Host-mapped buffers must be HOST_VISIBLE and HOST_COHERENT: nothing here flushes or
 * invalidates mapped ranges. A buffer the GPU copies into for the CPU to read (a readback)
 * also prefers HOST_CACHED. The first visible+coherent type is usually uncached
 * write-combined memory, where every CPU read goes to memory: on an RTX 3080 that made the
 * per-frame framebuffer readback the largest single CPU cost of a measured title (#1296).
 * Without a cached+coherent type the plain visible+coherent choice is kept. */
uint32_t gegpu_pick_host_mem_type(const uint32_t *type_flags, uint32_t type_count,
                                  uint32_t allowed_bits, int cpu_reads) {
    const uint32_t visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const uint32_t cached = visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    uint32_t fallback = UINT32_MAX;
    for (uint32_t i = 0; i < type_count && i < 32u; i++) {
        if (!(allowed_bits & (1u << i))) continue;
        if (cpu_reads && (type_flags[i] & cached) == cached) return i;
        if ((type_flags[i] & visible) == visible && fallback == UINT32_MAX) {
            if (!cpu_reads) return i;
            fallback = i;
        }
    }
    return fallback;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf,
                       VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = usage;
    VKC(vkCreateBuffer(s_dev, &bci, NULL, buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, *buf, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s_pdev, &mp);
    uint32_t type_flags[VK_MAX_MEMORY_TYPES];
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) type_flags[i] = mp.memoryTypes[i].propertyFlags;
    mai.memoryTypeIndex = gegpu_pick_host_mem_type(type_flags, mp.memoryTypeCount,
        mr.memoryTypeBits, (usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT) != 0);
    VKC(vkAllocateMemory(s_dev, &mai, NULL, mem));
    VKC(vkBindBufferMemory(s_dev, *buf, *mem, 0));
    if (map) VKC(vkMapMemory(s_dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
    return 1;
}

static int make_image_levels(uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt,
                             VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent.width = w; ici.extent.height = h; ici.extent.depth = 1;
    ici.mipLevels = levels; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(s_dev, &ici, NULL, img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_dev, *img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(s_dev, &mai, NULL, mem));
    VKC(vkBindImageMemory(s_dev, *img, *mem, 0));
    return 1;
}

static int make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImage *img, VkDeviceMemory *mem) {
    return make_image_levels(w, h, 1u, fmt, usage, img, mem);
}

static int make_view_levels(VkImage img, VkFormat fmt, VkImageAspectFlags aspect,
                            uint32_t levels, VkImageView *view) {
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = levels; vci.subresourceRange.layerCount = 1;
    VKC(vkCreateImageView(s_dev, &vci, NULL, view));
    return 1;
}

static int make_view(VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkImageView *view) {
    return make_view_levels(img, fmt, aspect, 1u, view);
}

/* Transition an image whose layout we track to `want` (full barrier; correctness over
 * fine-grained sync — these happen a handful of times per frame). Every mip level moves
 * together: a texture's chain (#1373) is uploaded and sampled as a whole. */
static void to_layout(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                      VkImageLayout *cur, VkImageLayout want) {
    if (*cur == want) return;
    VkImageMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.oldLayout = *cur; mb.newLayout = want;
    mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange.aspectMask = aspect;
    mb.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    mb.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, NULL, 0, NULL, 1, &mb);
    *cur = want;
}

static int cmd_batch_flush(void);
static int cmd_submit_wait(SrPerfGeReason reason);

static GeGpuReplayBoundaryKind boundary_for_reason(SrPerfGeReason reason) {
    switch (reason) {
        case SR_PERF_GE_RENDER_BATCH:
        case SR_PERF_GE_SNAPSHOT_COPY:
        case SR_PERF_GE_MIXED_DRAIN:
            return GEGPU_BOUNDARY_RENDER_SNAPSHOT;
        case SR_PERF_GE_TEXTURE_UPLOAD: return GEGPU_BOUNDARY_TEXTURE_UPLOAD;
        case SR_PERF_GE_TARGET_UPLOAD: return GEGPU_BOUNDARY_TARGET_UPLOAD;
        case SR_PERF_GE_DEPTH_UPLOAD: return GEGPU_BOUNDARY_DEPTH_UPLOAD;
        case SR_PERF_GE_DEPTH_READBACK:
        case SR_PERF_GE_TARGET_READBACK_TRANSITION:
            return GEGPU_BOUNDARY_READBACK;
        default: return GEGPU_BOUNDARY_OTHER;
    }
}

static void replay_note_submit(SrPerfGeReason reason, uint64_t elapsed) {
    GeGpuReplayBoundaryStats *b = &s_replay_stats.boundary[boundary_for_reason(reason)];
    b->submits++;
    b->submit_ns += elapsed;
    s_replay_stats.queue_submits++;
    if (s_cpu_profile_hook_depth > 0) s_cpu_profile_stats.hook_submit_ns += elapsed;
}

static void replay_note_wait(GeGpuReplayBoundaryKind boundary, uint64_t elapsed) {
    if ((unsigned)boundary >= GEGPU_BOUNDARY_COUNT) boundary = GEGPU_BOUNDARY_OTHER;
    s_replay_stats.boundary[boundary].waits++;
    s_replay_stats.boundary[boundary].wait_ns += elapsed;
    if (s_cpu_profile_hook_depth > 0) s_cpu_profile_stats.hook_wait_ns += elapsed;
}

static int cmd_wait_slot(SubmitSlot *slot, GeGpuReplayBoundaryKind boundary) {
    if (!slot || !slot->in_flight) return 1;
    uint64_t perf_started = sr_perf_now_ns();
    uint64_t wait_started = SDL_GetTicksNS();
    VKC(vkWaitForFences(s_dev, 1, &slot->fence, VK_TRUE, UINT64_MAX));
    uint64_t waited = SDL_GetTicksNS() - wait_started;
    replay_note_wait(boundary, waited);
    if (perf_started) {
        sr_perf_vulkan_wait(perf_started, slot->reason == SR_PERF_GE_DEPTH_READBACK ||
                                       slot->reason == SR_PERF_GE_TARGET_READBACK_TRANSITION);
        sr_perf_ge_wait(perf_started, slot->reason);
    }
    if (slot->reason == SR_PERF_GE_RENDER_BATCH) {
        s_replay_stats.render_waits++;
        s_replay_stats.render_wait_ns += waited;
    } else if (slot->reason == SR_PERF_GE_SNAPSHOT_COPY) {
        s_replay_stats.snapshot_waits++;
        s_replay_stats.snapshot_wait_ns += waited;
    }
    VKC(vkResetFences(s_dev, 1, &slot->fence));
    slot->in_flight = 0;
    slot->reason_mask = 0;
    slot->recorded_ops = 0;
    slot->vused = 0;
    slot->tused = slot->pused = 0;
    slot->xfer_used = 0;
    return 1;
}

static int cmd_drain(GeGpuReplayBoundaryKind boundary) {
    if (!cmd_batch_flush()) return 0;
    VkFence fences[SUBMIT_FRAMES];
    int slots[SUBMIT_FRAMES];
    uint32_t count = 0;
    SrPerfGeReason wait_reason = SR_PERF_GE_REASON_COUNT;
    for (int i = 0; i < SUBMIT_FRAMES; i++) {
        if (!s_submit[i].in_flight) continue;
        fences[count] = s_submit[i].fence;
        slots[count++] = i;
        if (wait_reason == SR_PERF_GE_REASON_COUNT) wait_reason = s_submit[i].reason;
        else if (wait_reason != s_submit[i].reason) wait_reason = SR_PERF_GE_MIXED_DRAIN;
    }
    if (!count) return 1;

    /* A single wait-all is sufficient at a CPU-visible boundary. All work is on
     * one queue, so it also preserves render -> copy -> next-render ordering. */
    uint64_t perf_started = sr_perf_now_ns();
    uint64_t wait_started = SDL_GetTicksNS();
    VKC(vkWaitForFences(s_dev, count, fences, VK_TRUE, UINT64_MAX));
    uint64_t waited = SDL_GetTicksNS() - wait_started;
    replay_note_wait(boundary, waited);
    if (perf_started) {
        sr_perf_vulkan_wait(perf_started, wait_reason == SR_PERF_GE_DEPTH_READBACK ||
                                       wait_reason == SR_PERF_GE_TARGET_READBACK_TRANSITION);
        sr_perf_ge_wait(perf_started, wait_reason);
    }
    if (wait_reason == SR_PERF_GE_RENDER_BATCH) {
        s_replay_stats.render_waits++;
        s_replay_stats.render_wait_ns += waited;
    } else if (wait_reason == SR_PERF_GE_SNAPSHOT_COPY) {
        s_replay_stats.snapshot_waits++;
        s_replay_stats.snapshot_wait_ns += waited;
    } else {
        s_replay_stats.mixed_waits++;
        s_replay_stats.mixed_wait_ns += waited;
    }
    VKC(vkResetFences(s_dev, count, fences));
    for (uint32_t i = 0; i < count; i++) {
        SubmitSlot *slot = &s_submit[slots[i]];
        slot->in_flight = 0;
        slot->reason_mask = 0;
        slot->recorded_ops = 0;
        slot->vused = 0;
        slot->tused = slot->pused = 0;
        slot->xfer_used = 0;
    }
    return 1;
}

/* Start a fresh scoped command buffer. The selected slot cannot be reset until its fence
 * retires, which also protects every vertex-arena range previously submitted in it. */
static int cmd_begin_fresh(void) {
    SubmitSlot *slot = &s_submit[s_async_submit ? s_submit_cursor : 0];
    if (!cmd_wait_slot(slot, GEGPU_BOUNDARY_OTHER)) return 0;
    /* A synchronous boundary can retire earlier queue work without acquiring every
     * signaled GE slot. Starting a new recording always owns a fresh arena logically,
     * even when this particular fence was already marked retired. */
    slot->reason_mask = 0;
    slot->recorded_ops = 0;
    slot->vused = 0;
    slot->tused = slot->pused = 0;
    slot->xfer_used = 0;
    s_cmd_slot = slot;
    s_cmd = slot->cmd;
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(s_cmd, &bi));
    return 1;
}

static int cmd_finish_submit(SrPerfGeReason reason, int defer) {
    SubmitSlot *slot = s_cmd_slot;
    if (!slot) return 0;
    VKC(vkEndCommandBuffer(s_cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd;
    uint64_t submit_started = SDL_GetTicksNS();
    uint64_t perf_submit_started = sr_perf_now_ns();
    VKC(vkQueueSubmit(s_queue, 1, &si, slot->fence));
    if (perf_submit_started) sr_perf_vulkan_submit(perf_submit_started);
    replay_note_submit(reason, SDL_GetTicksNS() - submit_started);
    if (sr_perf_enabled) sr_perf_ge_submit(reason);
    slot->reason = reason;
    slot->in_flight = 1;
    if (reason == SR_PERF_GE_RENDER_BATCH) s_replay_stats.render_submits++;
    else if (reason == SR_PERF_GE_SNAPSHOT_COPY) s_replay_stats.snapshot_submits++;
    else if (reason == SR_PERF_GE_MIXED_DRAIN) s_replay_stats.mixed_submits++;
    if (defer) s_submit_cursor = (s_submit_cursor + 1u) % SUBMIT_FRAMES;
    s_cmd_slot = NULL;
    s_cmd = VK_NULL_HANDLE;
    if (!defer && !cmd_wait_slot(slot, boundary_for_reason(reason))) return 0;
    return 1;
}

/* Submit the currently recorded render/snapshot chain. A mixed command buffer has one
 * physical queue-submit reason; its eventual wait is likewise reported as mixed. */
static int cmd_batch_flush(void) {
    if (!s_cmd_slot) return 1;
    SrPerfGeReason reason = SR_PERF_GE_MIXED_DRAIN;
    for (unsigned i = 0; i < SR_PERF_GE_REASON_COUNT; i++)
        if (s_cmd_slot->reason_mask == (1u << i)) { reason = (SrPerfGeReason)i; break; }
    return cmd_finish_submit(reason, s_async_submit);
}

/* CPU upload/readback operations and other callers of cmd_begin are boundaries. Submit
 * any open render/snapshot chain first; the boundary submission then waits on its own
 * fence, which also retires earlier work in the same queue. */
static int cmd_begin(void) {
    if (!cmd_batch_flush()) return 0;
    return cmd_begin_fresh();
}

/* `tverts` and `tparams` are the T&L arena space the op needs; tparams counts the records
 * from the start of the staged array, as submit_range may place them past the slot's cursor. */
static int cmd_batch_begin_tnl(uint32_t vertices, uint32_t tverts, uint32_t tparams) {
    if (vertices > VERT_ARENA_VERTS || tverts > TNL_ARENA_VERTS || tparams > TNL_ARENA_PARAMS)
        return 0;
    if (s_cmd_slot && (s_cmd_slot->recorded_ops >= s_submit_batch_ops ||
                       s_cmd_slot->vused + vertices > VERT_ARENA_VERTS ||
                       s_cmd_slot->tused + tverts > TNL_ARENA_VERTS ||
                       s_cmd_slot->pused + tparams > TNL_ARENA_PARAMS)) {
        if (!cmd_batch_flush()) return 0;
    }
    if (!s_cmd_slot && !cmd_begin_fresh()) return 0;
    return 1;
}

static int cmd_batch_begin(uint32_t vertices) { return cmd_batch_begin_tnl(vertices, 0u, 0u); }

static int cmd_batch_record(SrPerfGeReason reason) {
    if (!s_cmd_slot) return 0;
    s_cmd_slot->reason_mask |= 1u << reason;
    s_cmd_slot->recorded_ops++;
    if (!s_async_submit || s_cmd_slot->recorded_ops >= s_submit_batch_ops)
        return cmd_batch_flush();
    return 1;
}

typedef struct UploadReservation {
    VkBuffer buffer;
    VkDeviceSize offset;
    uint8_t *map;
    int batched;
} UploadReservation;

static VkDeviceSize align_up(VkDeviceSize value, VkDeviceSize alignment) {
    if (alignment <= 1) return value;
    return ((value + alignment - 1) / alignment) * alignment;
}

/* Reserve staging bytes owned by the current submit slot. The mapped range cannot be
 * reused until that slot's fence retires. A too-small configured arena takes the exact
 * shared-s_xfer synchronous fallback; ordinary exhaustion submits the open chain and
 * advances to another fenced slot. */
static int upload_reserve(VkDeviceSize bytes, UploadReservation *out) {
    memset(out, 0, sizeof(*out));
    if (!s_async_submit || !s_xfer_ring_bytes || bytes > s_xfer_ring_bytes) {
        if (!cmd_begin()) return 0;
        out->buffer = s_xfer;
        out->map = (uint8_t *)s_xfer_map;
        s_replay_stats.upload_ring_fallbacks++;
        return 1;
    }

    if (!s_cmd_slot && !cmd_begin_fresh()) return 0;
    VkDeviceSize offset = align_up(s_cmd_slot->xfer_used, s_xfer_align);
    if (s_cmd_slot->recorded_ops >= s_submit_batch_ops || offset + bytes > s_xfer_ring_bytes) {
        s_replay_stats.upload_ring_wraps++;
        if (!cmd_batch_flush() || !cmd_begin_fresh()) return 0;
        offset = 0;
    }
    if (offset + bytes > s_xfer_ring_bytes) return 0;

    s_cmd_slot->xfer_used = offset + bytes;
    if (s_cmd_slot->xfer_used > s_replay_stats.upload_ring_high_water)
        s_replay_stats.upload_ring_high_water = s_cmd_slot->xfer_used;
    s_replay_stats.upload_ring_reservations++;
    s_replay_stats.upload_ring_bytes += bytes;
    out->buffer = s_cmd_slot->xfer;
    out->offset = offset;
    out->map = s_cmd_slot->xfer_map + offset;
    out->batched = 1;
    return 1;
}

static int upload_finish(const UploadReservation *upload, SrPerfGeReason reason) {
    return upload->batched ? cmd_batch_record(reason) : cmd_submit_wait(reason);
}

static int cmd_submit_wait(SrPerfGeReason reason) {
    return cmd_finish_submit(reason, 0);
}

/* ---- guest framebuffer <-> RGBA8 (must match ge.c unpack_color / pack_fb exactly) ----- */

static uint32_t fb_unpack(uint32_t raw, uint32_t fmt) {
    uint32_t r, g, b, a;
    switch (fmt & 3) {
        case 0:
            r=raw&0x1F; g=(raw>>5)&0x3F; b=(raw>>11)&0x1F;
            r=(r<<3)|(r>>2); g=(g<<2)|(g>>4); b=(b<<3)|(b>>2); a=255;
            break;
        case 1:
            r=raw&0x1F; g=(raw>>5)&0x1F; b=(raw>>10)&0x1F;
            r=(r<<3)|(r>>2); g=(g<<3)|(g>>2); b=(b<<3)|(b>>2);
            a=(raw&0x8000)?255:0;
            break;
        case 2: r=(raw&0xF)*17; g=((raw>>4)&0xF)*17; b=((raw>>8)&0xF)*17; a=((raw>>12)&0xF)*17; break;
        default: return raw;
    }
    return r | (g << 8) | (b << 16) | (a << 24);
}

static uint32_t fb_pack(uint32_t px, uint32_t fmt) {
    uint32_t r = px & 0xFF, g = (px >> 8) & 0xFF, b = (px >> 16) & 0xFF, a = px >> 24;
    switch (fmt & 3) {
        case 0: return ((r>>3)&0x1F) | (((g>>2)&0x3F)<<5) | (((b>>3)&0x1F)<<11);
        case 1: return ((r>>3)&0x1F) | (((g>>3)&0x1F)<<5) | (((b>>3)&0x1F)<<10) | (a>=128?0x8000u:0);
        case 2: return ((r>>4)&0xF) | (((g>>4)&0xF)<<4) | (((b>>4)&0xF)<<8) | (((a>>4)&0xF)<<12);
        default: return px;
    }
}

/* Validate a complete framebuffer descriptor before any row is touched. The old
 * readback path checked only the first byte, so an apparently valid base near the
 * end of guest VRAM could wrap row-by-row through the mirrored aperture. */
int gegpu_validate_guest_fb_descriptor(const GeGpuFbDescriptor *desc,
                                       GeGpuFbSpan *out_span, const char **why) {
    const char *ignored = NULL;
    if (!why) why = &ignored;
    *why = NULL;
    if (!desc) { *why = "null descriptor"; return 0; }
    if (desc->format > 3u) { *why = "unsupported pixel format"; return 0; }
    uint32_t bpp = desc->format == 3u ? 4u : 2u;
    if (desc->width == 0u || desc->height == 0u) {
        *why = "zero visible width or height"; return 0;
    }
    if (desc->width > GEGPU_FB_MAX_STRIDE || desc->height > GEGPU_FB_MAX_HEIGHT) {
        *why = "visible extent above maximum"; return 0;
    }
    if (desc->stride == 0u || desc->stride > GEGPU_FB_MAX_STRIDE) {
        *why = desc->stride == 0u ? "zero buffer width" : "buffer width above maximum";
        return 0;
    }
    if (desc->stride < desc->width) {
        *why = "buffer width narrower than visible width"; return 0;
    }
    SrGuestRectSpan rect;
    if (!sr_guest_rect_geometry(desc->addr, 0u, 0u, desc->stride,
                                desc->width, desc->height, bpp, &rect) ||
        rect.total_bytes == 0u) {
        *why = "visible span overflows"; return 0;
    }

    int in_vram = is_vram(desc->addr);
    uint32_t voff = 0u, base = desc->addr;
    if (in_vram) {
        voff = vram_off(desc->addr);
        if (voff >= GEGPU_VRAM_BYTES || rect.total_bytes > GEGPU_VRAM_BYTES - voff) {
            *why = "span crosses the end of the VRAM aperture"; return 0;
        }
        base = 0x04000000u | voff;
    }
    if (!sr_guest_span_readable(base, rect.total_bytes)) {
        *why = "span leaves guest memory"; return 0;
    }
    if (out_span) {
        out_span->base = base;
        out_span->bytes_per_pixel = bpp;
        out_span->row_pitch = rect.row_pitch;
        out_span->total_bytes = rect.total_bytes;
        out_span->in_vram = in_vram;
        out_span->vram_offset = voff;
    }
    return 1;
}

static GeGpuFbDescriptor target_descriptor(uint32_t fba, uint32_t stride, uint32_t fmt) {
    GeGpuFbDescriptor d;
    d.addr = 0x04000000u | fba;
    d.format = fmt;
    d.stride = stride;
    d.width = stride < FB_W ? stride : FB_W;
    d.height = FB_H;
    return d;
}

/* `src` is the full SCALED readback (SCL_W x SCL_H); each guest pixel is written from
 * the top-left device subpixel of its scale x scale cell (identity at scale 1). */
static int write_guest_fb(const uint32_t *src, uint32_t fba, uint32_t stride, uint32_t fmt) {
    uint32_t srow = SCL_W;
    GeGpuFbDescriptor desc = target_descriptor(fba, stride, fmt);
    GeGpuFbSpan span;
    const char *why = NULL;
    if (!gegpu_validate_guest_fb_descriptor(&desc, &span, &why)) {
        fprintf(stderr, "gegpu: refusing framebuffer write fba=0x%08x stride=%u fmt=%u: %s\n",
                fba, stride, fmt, why ? why : "invalid descriptor");
        return 0;
    }
    uint32_t wb = desc.width;
    uint32_t rba = span.base;
    for (uint32_t y = 0; y < FB_H; y++) {
        const uint32_t *sl = src + (uint32_t)(y * s_scale) * srow;
        if ((fmt & 3) == 3) {
            uint32_t *dst = (uint32_t *)SR_HOST(rba + y * stride * 4);
            if (s_scale == 1) memcpy(dst, sl, wb * 4);
            else for (uint32_t x = 0; x < wb; x++) dst[x] = sl[x * s_scale];
        } else {
            uint16_t *dst = (uint16_t *)SR_HOST(rba + y * stride * 2);
            for (uint32_t x = 0; x < wb; x++)
                dst[x] = (uint16_t)fb_pack(sl[x * s_scale], fmt);
        }
    }
    return 1;
}

/* Returns 1 when retired, 0 while still in flight, and -1 on a Vulkan error. */
static int readback_finish(ReadbackSlot *r, int wait, int allow_commit) {
    if (!r->pending) return 1;
    uint64_t wait_started = wait ? sr_perf_now_ns() : 0;
    VkResult vr = wait ? vkWaitForFences(s_dev, 1, &r->fence, VK_TRUE, UINT64_MAX)
                       : vkGetFenceStatus(s_dev, r->fence);
    if (wait && wait_started) {
        sr_perf_vulkan_wait(wait_started, 1);
        sr_perf_ge_wait(wait_started, SR_PERF_GE_TARGET_READBACK_TRANSITION);
    }
    if (vr == VK_NOT_READY) return 0;
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "gegpu: readback fence failed: %d\n", (int)vr);
        s_readback_commit_failures++;
        return -1;
    }
    Target *t = r->target;
    int commit_failed = 0;
    if (allow_commit && r->commit && t && t->used && t->img == r->image &&
        t->gpu_valid && t->render_gen == r->gen) {
        if (write_guest_fb(r->map, r->fba, r->stride, r->fmt)) {
            t->clean_gen = r->gen;
            if (sr_perf_enabled) sr_perf_vulkan_readback((uint32_t)(SCL_W * SCL_H * 4u));
            s_cnt_readback++;
        } else {
            commit_failed = 1;
            s_readback_commit_failures++;
        }
    }
    r->pending = 0;
    r->commit = 0;
    r->target = NULL;
    r->image = VK_NULL_HANDLE;
    return commit_failed ? -1 : 1;
}

static void readback_poll(void) {
    for (int i = 0; i < READBACK_FRAMES; i++)
        (void)readback_finish(&s_readback[i], 0, 1);
}

static void readback_discard_target(Target *t) {
    for (int i = 0; i < READBACK_FRAMES; i++)
        if (s_readback[i].pending && s_readback[i].image == t->img)
            s_readback[i].commit = 0;
}

static int readback_wait_target(Target *t, int commit) {
    for (int i = 0; i < READBACK_FRAMES; i++) {
        ReadbackSlot *r = &s_readback[i];
        if (r->pending && r->image == t->img && readback_finish(r, 1, commit) < 0)
            return 0;
    }
    return 1;
}

static ReadbackSlot *readback_acquire(void) {
    readback_poll();
    for (int n = 0; n < READBACK_FRAMES; n++) {
        uint32_t i = (s_readback_cursor + (uint32_t)n) % READBACK_FRAMES;
        if (!s_readback[i].pending) {
            s_readback_cursor = (i + 1) % READBACK_FRAMES;
            return &s_readback[i];
        }
    }
    return NULL;
}

/* ---- pipelines ------------------------------------------------------------------------ */

static VkPipeline pipe_create(const PipeKey *k) {
    uint64_t profile_started = cpu_profile_now();
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = k->tnl ? s_vs_tnl : s_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s_fs; st[1].pName = "main";

    VkVertexInputBindingDescription bind = { 0, sizeof(GpuVert), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription at[4] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,       16 },
        { 2, 0, VK_FORMAT_R32_SFLOAT,          24 },
        { 3, 0, VK_FORMAT_R8G8B8A8_UNORM,      28 },
    };
    /* GeTnlVtx for psp_tnl.vert */
    VkVertexInputBindingDescription tbind = { 0, sizeof(GeTnlVtx), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription tat[7] = {
        { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    (uint32_t)offsetof(GeTnlVtx, x) },
        { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,    (uint32_t)offsetof(GeTnlVtx, nx) },
        { 2, 0, VK_FORMAT_R32G32_SFLOAT,       (uint32_t)offsetof(GeTnlVtx, u) },
        { 3, 0, VK_FORMAT_R8G8B8A8_UINT,       (uint32_t)offsetof(GeTnlVtx, rgba) },
        { 4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(GeTnlVtx, w) },
        { 5, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(GeTnlVtx, w) + 16u },
        { 6, 0, VK_FORMAT_R32_UINT,            (uint32_t)offsetof(GeTnlVtx, param) },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1;
    vi.pVertexBindingDescriptions = k->tnl ? &tbind : &bind;
    vi.vertexAttributeDescriptionCount = k->tnl ? 7u : 4u;
    vi.pVertexAttributeDescriptions = k->tnl ? tat : at;

    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = k->cull;
    /* raster_tri keeps POSITIVE cross product in y-down screen coords; Vulkan's signed
     * area has the opposite sign convention, so those triangles are Vulkan-"clockwise". */
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    ds.depthTestEnable = k->ztest;
    ds.depthWriteEnable = k->zwrite;
    ds.depthCompareOp = (VkCompareOp)k->zfunc;

    VkPipelineColorBlendAttachmentState ba = {0};
    ba.blendEnable = k->blend_on;
    ba.srcColorBlendFactor = (VkBlendFactor)k->srcf;
    ba.dstColorBlendFactor = (VkBlendFactor)k->dstf;
    ba.colorBlendOp = (VkBlendOp)k->eq;
    ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;    /* PSP: dst alpha = clamp(sa + da) */
    ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ba.alphaBlendOp = VK_BLEND_OP_ADD;
    ba.colorWriteMask = k->cmask;
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dn = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dn.dynamicStateCount = 3; dn.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount = 2; pci.pStages = st;
    pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp; pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb; pci.pDynamicState = &dn;
    pci.layout = s_playout; pci.renderPass = s_rp;

    VkPipeline p = VK_NULL_HANDLE;
    uint64_t perf_started = sr_perf_now_ns();
    VkResult r = vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &pci, NULL, &p);
    if (perf_started) sr_perf_vulkan_pipeline(perf_started);
    if (r != VK_SUCCESS) fprintf(stderr, "gegpu: pipeline create failed: %d\n", (int)r);
    if (s_cpu_profile) {
        cpu_profile_add(GEGPU_CPU_PIPELINE_CREATE, profile_started);
        if (r == VK_SUCCESS) s_cpu_profile_stats.pipeline_creations++;
    }
    return p;
}

static VkPipeline pipe_get(const PipeKey *k) {
    uint64_t profile_started = cpu_profile_now();
    for (int i = 0; i < s_pipe_n; i++) {
        if (!memcmp(&s_pipes[i].key, k, sizeof(*k))) {
            if (s_cpu_profile) {
                s_cpu_profile_stats.pipeline_hits++;
                cpu_profile_add(GEGPU_CPU_PIPELINE_LOOKUP, profile_started);
            }
            return s_pipes[i].pipe;
        }
    }
    if (s_cpu_profile) {
        s_cpu_profile_stats.pipeline_misses++;
        cpu_profile_add(GEGPU_CPU_PIPELINE_LOOKUP, profile_started);
    }
    /* A full cache grows instead of refusing the state: a refused pipeline used to drop
     * its whole batch silently once the cache held 192 distinct keys. */
    if (s_pipe_n == s_pipe_cap) {
        int cap = s_pipe_cap ? s_pipe_cap * 2 : PIPE_CACHE_INITIAL;
        PipeEnt *grown = (PipeEnt *)realloc(s_pipes, (size_t)cap * sizeof(*grown));
        if (!grown) return VK_NULL_HANDLE;
        s_pipes = grown;
        s_pipe_cap = cap;
    }
    VkPipeline p = pipe_create(k);
    if (!p) return VK_NULL_HANDLE;
    s_pipes[s_pipe_n].key = *k; s_pipes[s_pipe_n].pipe = p; s_pipe_n++;
    return p;
}

/* ---- descriptors ------------------------------------------------------------------------ */

/* Every set carries two bindings: 0 = the texture this set is for, 1 = the shared
 * destination-snapshot image (shader blending reads it; refreshed before such draws).
 * s_snap_view is created before the first make_descriptor call in gegpu_init. */
static VkDescriptorSet make_descriptor(VkImageView view, VkSampler smp, VkDescriptorPool pool) {
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    dai.descriptorPool = pool; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_dlayout;
    VkDescriptorSet set = VK_NULL_HANDLE;
    uint64_t alloc_started = cpu_profile_now();
    VkResult alloc_result = vkAllocateDescriptorSets(s_dev, &dai, &set);
    if (s_cpu_profile) {
        cpu_profile_add(GEGPU_CPU_DESCRIPTOR_ALLOC, alloc_started);
        if (alloc_result == VK_SUCCESS) s_cpu_profile_stats.descriptor_allocations++;
    }
    if (alloc_result != VK_SUCCESS) return VK_NULL_HANDLE;
    VkDescriptorImageInfo dii = { smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo dds = { s_smp[0], s_snap_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr[2] = {
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
    };
    wr[0].dstSet = set; wr[0].dstBinding = 0; wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[0].pImageInfo = &dii;
    wr[1].dstSet = set; wr[1].dstBinding = 1; wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[1].pImageInfo = &dds;
    uint64_t update_started = cpu_profile_now();
    vkUpdateDescriptorSets(s_dev, 2, wr, 0, NULL);
    if (s_cpu_profile) {
        cpu_profile_add(GEGPU_CPU_DESCRIPTOR_UPDATE, update_started);
        s_cpu_profile_stats.descriptor_updates += 2;
    }
    return set;
}

/* ---- batch submission (GPU only, no VRAM I/O) ------------------------------------------- */

static uint32_t s_flushgen = 1;            /* bumped per submit: invalidates state template */

static void batches_reset(void) {
    s_nbatch = 0; s_nverts = 0;
    s_ntnl = 0; s_ntnlp = 0;
    s_flushgen++;
    for (int i = 0; i < s_tex_n; i++) s_tex[i].pending = 0;
}

static const SnapRect k_snap_rect_empty = { 0, 0, 0, 0 };

static int snap_rect_empty(const SnapRect *r) { return r->x0 >= r->x1 || r->y0 >= r->y1; }

static void snap_rect_union(SnapRect *a, const SnapRect *b) {
    if (snap_rect_empty(b)) return;
    if (snap_rect_empty(a)) { *a = *b; return; }
    if (b->x0 < a->x0) a->x0 = b->x0;
    if (b->y0 < a->y0) a->y0 = b->y0;
    if (b->x1 > a->x1) a->x1 = b->x1;
    if (b->y1 > a->y1) a->y1 = b->y1;
}

static int snap_rect_overlap(const SnapRect *a, const SnapRect *b) {
    if (snap_rect_empty(a) || snap_rect_empty(b)) return 0;
    return a->x0 < b->x1 && b->x0 < a->x1 && a->y0 < b->y1 && b->y0 < a->y1;
}

/* Guest-pixel box holding every fragment of a batch: its vertices' extent widened by a pixel
 * on each side, clipped to the batch scissor and the framebuffer. Vertices are guest pixels
 * (psp.vert maps x/256 - 1, y/136 - 1), and a device fragment at render scale s lies in guest
 * pixel floor(device / s), which the widened box contains. */
static SnapRect verts_rect(const Batch *b, uint32_t first, uint32_t count) {
    SnapRect r = k_snap_rect_empty;
    if (!count) return r;
    const GpuVert *v = &s_vcpu[first];
    float x0 = v[0].x, x1 = v[0].x, y0 = v[0].y, y1 = v[0].y;
    for (uint32_t i = 1; i < count; i++) {
        if (v[i].x < x0) x0 = v[i].x;
        if (v[i].x > x1) x1 = v[i].x;
        if (v[i].y < y0) y0 = v[i].y;
        if (v[i].y > y1) y1 = v[i].y;
    }
    int sx0 = b->sx, sy0 = b->sy, sx1 = b->sx + b->sw, sy1 = b->sy + b->sh;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > FB_W) sx1 = FB_W;
    if (sy1 > FB_H) sy1 = FB_H;
    /* Clamp in float before converting: a guard-band vertex may lie far outside int range. */
    float fx0 = floorf(x0) - 1.0f, fy0 = floorf(y0) - 1.0f;
    float fx1 = ceilf(x1) + 2.0f, fy1 = ceilf(y1) + 2.0f;
    r.x0 = fx0 <= (float)sx0 ? sx0 : (fx0 >= (float)sx1 ? sx1 : (int)fx0);
    r.y0 = fy0 <= (float)sy0 ? sy0 : (fy0 >= (float)sy1 ? sy1 : (int)fy0);
    r.x1 = fx1 >= (float)sx1 ? sx1 : (fx1 <= (float)sx0 ? sx0 : (int)fx1);
    r.y1 = fy1 >= (float)sy1 ? sy1 : (fy1 <= (float)sy0 ? sy0 : (int)fy1);
    return r;
}

/* A T&L batch's box: the union of its PRIMs' screen boxes (ge.c's clip-space bound, which
 * holds every vertex), clamped to the scissor like verts_rect. */
static SnapRect tnl_rect(const Batch *b, uint32_t first, uint32_t count) {
    SnapRect r = k_snap_rect_empty;
    if (!count) return r;
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    /* The boxes are in staging order, so by first reference: find the first PRIM that ends
     * past `first`, then walk while PRIMs start before the range's end. */
    uint32_t lo = 0, hi = s_ntnlp;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (s_tnl_box[mid].end <= first) lo = mid + 1u; else hi = mid;
    }
    for (uint32_t i = lo; i < s_ntnlp && s_tnl_box[i].first < first + count; i++) {
        const TnlBox *t = &s_tnl_box[i];
        if (t->x0 < x0) x0 = t->x0;
        if (t->y0 < y0) y0 = t->y0;
        if (t->x1 > x1) x1 = t->x1;
        if (t->y1 > y1) y1 = t->y1;
    }
    if (x0 > x1) return r;
    int sx0 = b->sx, sy0 = b->sy, sx1 = b->sx + b->sw, sy1 = b->sy + b->sh;
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 > FB_W) sx1 = FB_W;
    if (sy1 > FB_H) sy1 = FB_H;
    float fx0 = floorf(x0) - 1.0f, fy0 = floorf(y0) - 1.0f;
    float fx1 = ceilf(x1) + 2.0f, fy1 = ceilf(y1) + 2.0f;
    r.x0 = fx0 <= (float)sx0 ? sx0 : (fx0 >= (float)sx1 ? sx1 : (int)fx0);
    r.y0 = fy0 <= (float)sy0 ? sy0 : (fy0 >= (float)sy1 ? sy1 : (int)fy0);
    r.x1 = fx1 >= (float)sx1 ? sx1 : (fx1 <= (float)sx0 ? sx0 : (int)fx1);
    r.y1 = fy1 >= (float)sy1 ? sy1 : (fy1 <= (float)sy0 ? sy0 : (int)fy1);
    return r;
}

static SnapRect batch_rect(const Batch *b) {
    return b->key.tnl ? tnl_rect(b, b->first, b->count) : verts_rect(b, b->first, b->count);
}

static int batch_reads_dst(const Batch *b) {
    return (((uint32_t)b->pc.cfg[0] >> 8) & (F_SHBLEND | F_STENCIL)) != 0;
}

/* Copy target `t` into the snapshot image in the open command buffer: the one copy recipe of
 * snapshot_refresh and submit_pending.
 *
 * When the snapshot already holds `t` and the dirty box accounts for everything drawn into it
 * since (s_snap_dirty_gen is t's current render_gen), only that box is copied: outside it the
 * snapshot already equals the target, so the result is the same image a full copy gives. Any
 * other state copies the whole target. A full copy moves scale^2 times the guest frame; a
 * title that blends against the destination a hundred times a frame spent its frame time on
 * those copies at 3x and 4x. */
static void snapshot_copy_record(Target *t) {
    SnapRect r = { 0, 0, FB_W, FB_H };
    if (s_snap_src == t && s_snap_tracked && s_snap_dirty_gen == t->render_gen &&
        !snap_rect_empty(&s_snap_dirty)) {
        r = s_snap_dirty;
        if (r.x0 < 0) r.x0 = 0;
        if (r.y0 < 0) r.y0 = 0;
        if (r.x1 > FB_W) r.x1 = FB_W;
        if (r.y1 > FB_H) r.y1 = FB_H;
        if (snap_rect_empty(&r)) r = (SnapRect){ 0, 0, FB_W, FB_H };
    }
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = {0};
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.srcOffset.x = r.x0 * s_scale; ic.srcOffset.y = r.y0 * s_scale;
    ic.dstOffset = ic.srcOffset;
    ic.extent.width = (uint32_t)((r.x1 - r.x0) * s_scale);
    ic.extent.height = (uint32_t)((r.y1 - r.y0) * s_scale);
    ic.extent.depth = 1;
    vkCmdCopyImage(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   s_snapimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    s_replay_stats.snapshot_copy_px += (unsigned long long)ic.extent.width * ic.extent.height;
    to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

/* The snapshot now holds exactly `t`. */
static void snapshot_note_copied(Target *t) {
    s_snap_src = t;
    s_snap_srcgen = t->render_gen;
    s_snap_dirty = k_snap_rect_empty;
    s_snap_dirty_gen = t->render_gen;
    s_snap_tracked = 1;
}

/* Would a shader-blend batch with box `box`, drawn after the not yet recorded `before`, read
 * anything from the snapshot that a fresh copy of `t` would not hold? */
static int snapshot_stale_for(const Target *t, const SnapRect *box, const SnapRect *before) {
    if (s_snap_src != t || !s_snap_tracked || s_snap_dirty_gen != t->render_gen) return 1;
    SnapRect dirty = s_snap_dirty;
    snap_rect_union(&dirty, before);
    return snap_rect_overlap(box, &dirty);
}

/* One destination-snapshot copy op, recorded on the GE chain like snapshot_refresh's. */
static int snapshot_copy_op(Target *t) {
    if (!cmd_batch_begin(0)) return 0;
    uint64_t region_started = cpu_profile_now();
    snapshot_copy_record(t);
    cpu_profile_add(GEGPU_CPU_SNAPSHOT_REGION, region_started);
    if (!cmd_batch_record(SR_PERF_GE_SNAPSHOT_COPY)) return 0;
    s_replay_stats.snapshot_copies++;
    if (s_cpu_profile) s_cpu_profile_stats.snapshot_copies++;
    if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SNAPSHOT_COPIED, 1);
    snapshot_note_copied(t);
    s_cnt_snap++;
    return 1;
}

/* Record batches [b0, b1) as one render op into `t`. Leaves the color image SHADER_READ_ONLY. */
static int submit_range(Target *t, uint32_t b0, uint32_t b1) {
    /* Captured vertices and T&L references are two arrays; batches of each kind are in
     * order within it. */
    uint32_t v0 = UINT32_MAX, v1 = 0, tv0 = UINT32_MAX, tv1 = 0;
    for (uint32_t i = b0; i < b1; i++) {
        const Batch *b = &s_batch[i];
        uint32_t *lo = b->key.tnl ? &tv0 : &v0, *hi = b->key.tnl ? &tv1 : &v1;
        if (b->first < *lo) *lo = b->first;
        if (b->first + b->count > *hi) *hi = b->first + b->count;
    }
    if (v0 == UINT32_MAX) v0 = v1 = 0;
    if (tv0 == UINT32_MAX) tv0 = tv1 = 0;
    const uint32_t nverts = v1 - v0, ntverts = tv1 - tv0;
    /* The parameter records the range's references name: those of the PRIMs it holds. */
    uint32_t p0 = UINT32_MAX, p1 = 0;
    for (uint32_t i = 0; ntverts && i < s_ntnlp; i++) {
        if (s_tnl_box[i].end <= tv0 || s_tnl_box[i].first >= tv1) continue;
        if (i < p0) p0 = i;
        p1 = i + 1u;
    }
    if (p0 == UINT32_MAX) p0 = p1 = 0;
    if (!cmd_batch_begin_tnl(nverts, ntverts, p1)) return 0;
    uint32_t vertex_base = s_cmd_slot->vused;
    uint32_t tvertex_base = s_cmd_slot->tused;
    /* Records go where their staged index plus a non-negative delta lands, and the delta is
     * the draws' firstInstance, which psp_tnl.vert adds to each reference's index. */
    uint32_t param_base = s_cmd_slot->pused > p0 ? s_cmd_slot->pused : p0;
    uint64_t memcpy_started = cpu_profile_now();
    memcpy(s_cmd_slot->vmap + vertex_base, s_vcpu + v0, (size_t)nverts * sizeof(GpuVert));
    if (ntverts) {
        memcpy(s_cmd_slot->tmap + tvertex_base, s_tnl_vcpu + tv0, (size_t)ntverts * sizeof(GeTnlVtx));
        memcpy(s_cmd_slot->pmap + param_base, s_tnl_pcpu + p0, (size_t)(p1 - p0) * sizeof(GeTnlParams));
    }
    if (s_cpu_profile) {
        cpu_profile_add(GEGPU_CPU_MEMCPY, memcpy_started);
        s_cpu_profile_stats.memcpy_bytes += (uint64_t)nverts * sizeof(GpuVert);
    }

    uint64_t command_started = cpu_profile_now();
    uint64_t nested_before = 0;
    if (s_cpu_profile)
        nested_before = s_cpu_profile_stats.phase[GEGPU_CPU_PIPELINE_LOOKUP].ns +
                        s_cpu_profile_stats.phase[GEGPU_CPU_PIPELINE_CREATE].ns +
                        s_cpu_profile_stats.phase[GEGPU_CPU_BIND_RECORD].ns;

    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    to_layout(s_cmd, t->dep->img, VK_IMAGE_ASPECT_DEPTH_BIT, &t->dep->layout,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);

    VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = s_rp; rbi.framebuffer = t->fb;
    rbi.renderArea.extent.width = SCL_W; rbi.renderArea.extent.height = SCL_H;
    vkCmdBeginRenderPass(s_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    /* the vertex NDC transform is resolution-independent: scaling the viewport IS the
     * internal-resolution upscale */
    VkViewport vp = { 0, 0, (float)SCL_W, (float)SCL_H, 0.0f, 1.0f };
    vkCmdSetViewport(s_cmd, 0, 1, &vp);
    VkDeviceSize vertex_offset = (VkDeviceSize)vertex_base * sizeof(GpuVert);
    VkDeviceSize tvertex_offset = (VkDeviceSize)tvertex_base * sizeof(GeTnlVtx);
    int bound_tnl = -1;
    if (ntverts)
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 1, 1,
                                &s_cmd_slot->tset, 0, NULL);

    VkPipeline cur = VK_NULL_HANDLE;
    VkDescriptorSet cur_set = VK_NULL_HANDLE;
    for (uint32_t i = b0; i < b1; i++) {
        Batch *b = &s_batch[i];
        VkPipeline p = pipe_get(&b->key);
        if (!p) {
            /* Not expected once the cache grows; say so rather than dropping silently. */
            static int pipe_drop_reported = 0;
            if (!pipe_drop_reported) {
                pipe_drop_reported = 1;
                fprintf(stderr, "gegpu: pipeline creation failed; a batch was dropped\n");
            }
            continue;
        }
        if (p != cur) {
            uint64_t bind_started = cpu_profile_now();
            vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
            if (s_cpu_profile) {
                cpu_profile_add(GEGPU_CPU_BIND_RECORD, bind_started);
                s_cpu_profile_stats.pipeline_binds++;
            }
            cur = p;
        } else if (s_cpu_profile) {
            s_cpu_profile_stats.pipeline_bind_redundant++;
        }
        VkRect2D sc = { { b->sx * s_scale, b->sy * s_scale },
                        { (uint32_t)(b->sw * s_scale), (uint32_t)(b->sh * s_scale) } };
        vkCmdSetScissor(s_cmd, 0, 1, &sc);
        vkCmdSetBlendConstants(s_cmd, b->bconst);
        vkCmdPushConstants(s_cmd, s_playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(b->pc), &b->pc);
        uint64_t bind_started = cpu_profile_now();
        vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1, &b->dset, 0, NULL);
        if (s_cpu_profile) {
            cpu_profile_add(GEGPU_CPU_BIND_RECORD, bind_started);
            s_cpu_profile_stats.descriptor_binds++;
            if (b->dset == cur_set) s_cpu_profile_stats.descriptor_bind_redundant++;
        }
        cur_set = b->dset;
        if (bound_tnl != (int)b->key.tnl) {
            vkCmdBindVertexBuffers(s_cmd, 0, 1, b->key.tnl ? &s_cmd_slot->tbuf : &s_cmd_slot->vbuf,
                                   b->key.tnl ? &tvertex_offset : &vertex_offset);
            bound_tnl = (int)b->key.tnl;
        }
        if (b->key.tnl)
            vkCmdDraw(s_cmd, b->count, 1, b->first - tv0, param_base - p0);
        else
            vkCmdDraw(s_cmd, b->count, 1, b->first - v0, 0);
    }
    vkCmdEndRenderPass(s_cmd);
    if (s_cpu_profile) {
        uint64_t total = SDL_GetTicksNS() - command_started;
        uint64_t nested_after = s_cpu_profile_stats.phase[GEGPU_CPU_PIPELINE_LOOKUP].ns +
                                s_cpu_profile_stats.phase[GEGPU_CPU_PIPELINE_CREATE].ns +
                                s_cpu_profile_stats.phase[GEGPU_CPU_BIND_RECORD].ns;
        uint64_t nested = nested_after - nested_before;
        s_cpu_profile_stats.phase[GEGPU_CPU_COMMAND_RECORD].calls++;
        if (s_cpu_profile_hook_depth > 0)
            s_cpu_profile_stats.hook_phase[GEGPU_CPU_COMMAND_RECORD].calls++;
        cpu_profile_add_elapsed(GEGPU_CPU_COMMAND_RECORD, total > nested ? total - nested : 0);
    }
    /* render pass final layouts (see init): color -> SHADER_READ_ONLY, depth stays */
    t->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    s_replay_stats.batches += b1 - b0;
    s_replay_stats.draws += b1 - b0;
    s_cmd_slot->vused += nverts;
    s_cmd_slot->tused += ntverts;
    if (ntverts) s_cmd_slot->pused = param_base + (p1 - p0);
    int ok = cmd_batch_record(SR_PERF_GE_RENDER_BATCH);
    t->gpu_valid = 1;
    t->render_gen++;
    if (t->dep) t->dep->guest_dirty = 1;
    sr_vram_mark_renderer_pending();
    s_cnt_submit++;
    if (s_stats) {
        s_cnt_batches += b1 - b0;
        s_cnt_verts += nverts;
    }
    for (uint32_t i = b0; i < b1; i++) {
        if (batch_reads_dst(&s_batch[i]) && sr_perf_enabled)
            sr_perf_ge_event(SR_PERF_GE_SHBLEND_BATCH, 1);
    }
    if (s_log)
        fprintf(stderr, "GEGPU submit #%lu batches=%u verts=%u tgt=0x%08x/%u fmt=%u\n",
                s_cnt_submit, b1 - b0, nverts, t->fba, t->stride, t->fmt);
    /* NOTE: prior revisions wrote MEM[0x331b80]=1 here to fake the GE
     * list-end interrupt and shared a state with mn_replay or sched.c to
     * drive the engine frame-counter. That was a side-effect of a missing
     * GE list-complete-callback path in the recomp; solving it there is
     * the proper fix, so we no longer poke the flag from submit_pending.
     * The engine's is-frame-ready latch (0x331b80) must now be flipped
     * by the recomp's list-complete CB walk (ge.c::GE_FINISH_CB -> func 0x599c).
     * Until that lands, the worker's inner spin on the counter stays --
     * intended, so the next debug session sees the real gap instead of a
     * worked-around symptom. */
    return ok;
}

/* Render all pending batches into s_cur, as render ops split only where a shader-blend batch
 * needs a fresh destination snapshot (snapshot_stale_for). The copy is recorded between the
 * batches before that batch and the batch itself, which is what a copy taken when its state was
 * built held; every other shader-blend batch reads the snapshot unchanged, because nothing drawn
 * since its copy touches the pixels it reads. Leaves the color image SHADER_READ_ONLY. */
static int submit_pending(void) {
    if (!s_nbatch || !s_cur) { if (s_nbatch == 0) batches_reset(); return 1; }
    Target *t = s_cur;
    int ok = 1;
    uint32_t i = 0;
    while (ok && i < s_nbatch) {
        SnapRect box = batch_rect(&s_batch[i]);
        if (batch_reads_dst(&s_batch[i])) {
            if (snapshot_stale_for(t, &box, &k_snap_rect_empty)) {
                if (!snapshot_copy_op(t)) { ok = 0; break; }
            } else if (sr_perf_enabled) {
                sr_perf_ge_event(SR_PERF_GE_SNAPSHOT_CACHE_HIT, 1);
            }
        }
        SnapRect drawn = box;
        uint32_t j = i + 1u;
        for (; j < s_nbatch; j++) {
            box = batch_rect(&s_batch[j]);
            if (batch_reads_dst(&s_batch[j])) {
                if (snapshot_stale_for(t, &box, &drawn)) break;
                if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SNAPSHOT_CACHE_HIT, 1);
            }
            snap_rect_union(&drawn, &box);
        }
        const int tracked = s_snap_src == t && s_snap_tracked && s_snap_dirty_gen == t->render_gen;
        ok = submit_range(t, i, j);
        if (ok && tracked) {
            /* Outside what this op drew, the snapshot still holds the target. */
            snap_rect_union(&s_snap_dirty, &drawn);
            s_snap_dirty_gen = t->render_gen;
            s_snap_srcgen = snap_rect_empty(&s_snap_dirty) ? t->render_gen : 0;
        }
        i = j;
    }
    batches_reset();
    return ok;
}

static void stats_emit(int final) {
    static unsigned long last_ms = 0;
    if (!s_stats) return;
    unsigned long now = (unsigned long)(clock() * 1000ull / CLOCKS_PER_SEC);
    if (!final && now - last_ms < 5000) return;
    last_ms = now;
    fprintf(stderr, "GEGPU stats: submits=%lu batches=%llu verts=%llu avg_batches=%.2f "
            "tris=%lu spr=%lu lines=%lu nonfinite=%lu tnl[prims=%lu tris=%lu] rtt=%lu snap=%lu "
            "present[gpu=%lu cpu=%lu] upload=%lu readback=%lu texup=%lu dirty=%lu xferblit=%lu pipes=%d texs=%d "
            "shadow[invalidations=%llu checks=%llu hits=%llu misses=%llu bytes=%llu avoided=%llu required=%llu] final=%d\n",
            s_cnt_submit, (unsigned long long)s_cnt_batches, (unsigned long long)s_cnt_verts,
            s_cnt_submit ? (double)s_cnt_batches / (double)s_cnt_submit : 0.0,
            s_cnt_tri, s_cnt_spr, s_cnt_line, s_cnt_nonfinite, s_cnt_tnl_prims, s_cnt_tnl_tris,
            s_cnt_rtt, s_cnt_snap,
            s_cnt_present_gpu, s_cnt_present_cpu, s_cnt_upload, s_cnt_readback, s_cnt_texup,
            s_cnt_dirty, s_cnt_xferblit, s_pipe_n, s_tex_n,
            (unsigned long long)s_cnt_tex_invalidations,
            (unsigned long long)s_cnt_shadow_checks, (unsigned long long)s_cnt_shadow_hits,
            (unsigned long long)s_cnt_shadow_misses, (unsigned long long)s_cnt_shadow_bytes,
            (unsigned long long)s_cnt_shadow_avoided, (unsigned long long)s_cnt_shadow_required,
            final);
}

static void stats_tick(void) { stats_emit(0); }

/* ---- targets ----------------------------------------------------------------------------- */

/* Write the target's GPU contents back to guest VRAM (eviction, CLUT-from-VRAM). */
static int target_sync_cpu_writes(Target *t);

static int target_readback(Target *t) {
    if (!t->gpu_valid) return 1;
    if (!target_sync_cpu_writes(t)) return 0;
    if (!t->gpu_valid) return 1;
    /* The pending batch holds the pixels we are about to materialize. Treat a failed
     * submit as a hard readback failure instead of blessing the previous generation. */
    if (t == s_cur && s_nbatch && !submit_pending()) {
        fprintf(stderr, "gegpu: target readback abandoned: pending batch did not submit\n");
        return 0;
    }
    readback_poll();
    if (t->clean_gen == t->render_gen) return 1;   /* VRAM already current */
    /* A presentation copy of this exact generation may already be in flight.  Waiting
     * for that target-local fence avoids issuing a duplicate copy. */
    for (int i = 0; i < READBACK_FRAMES; i++) {
        ReadbackSlot *r = &s_readback[i];
        if (r->pending && r->image == t->img && r->gen == t->render_gen) {
            if (readback_finish(r, 1, 1) < 0) return 0;
            if (t->clean_gen == t->render_gen) return 1;
        }
    }
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_xfer, 1, &c);
    if (!cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION)) return 0;
    if (!write_guest_fb((const uint32_t *)s_xfer_map, t->fba, t->stride, t->fmt)) return 0;
    uint32_t bpp = t->fmt == 3u ? 4u : 2u;
    ge_zbuf_invalidate_guest_span(0x04000000u + t->fba,
                                  t->stride * FB_H * bpp);
    t->clean_gen = t->render_gen;
    s_cnt_readback++;
    return 1;
}

/* Queue the image transition and optional guest-VRAM copy needed by presentation.
 * Nothing waits here: sdl3vk submits the blit to the same queue, so queue order makes
 * TRANSFER_SRC visible before it is consumed. */
static int target_prepare_present(Target *t) {
    if (!t || !t->gpu_valid) return 0;
    if (t == s_cur && s_nbatch && !submit_pending()) return 0;
    /* Presentation/readback command buffers are submitted outside the GE slot ring.
     * Close the GE chain first so their same-queue order reflects program order. */
    if (!cmd_batch_flush()) return 0;
    readback_poll();

    int copy = t->clean_gen != t->render_gen;
    if (t->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && !copy) return 1;
    if (copy) {
        for (int i = 0; i < READBACK_FRAMES; i++) {
            ReadbackSlot *r = &s_readback[i];
            if (r->pending && r->image == t->img && r->gen == t->render_gen &&
                t->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
                return 1;
        }
    }

    ReadbackSlot *r = readback_acquire();
    if (!r) {
        /* The ring being full is exceptional.  Preserve correctness with a scoped
         * target readback/transition instead of a device-wide drain. */
        if (copy) return target_readback(t);
        if (!cmd_begin()) return 0;
        to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        return cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION);
    }

    VkImageLayout old_layout = t->layout;
    if (vkResetCommandBuffer(r->cmd, 0) != VK_SUCCESS) return 0;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(r->cmd, &bi) != VK_SUCCESS) return 0;
    to_layout(r->cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    if (copy) {
        VkBufferImageCopy c = {0};
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
        vkCmdCopyImageToBuffer(r->cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r->buf, 1, &c);
    }
    if (vkEndCommandBuffer(r->cmd) != VK_SUCCESS ||
        vkResetFences(s_dev, 1, &r->fence) != VK_SUCCESS) {
        t->layout = old_layout;
        return 0;
    }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &r->cmd;
    uint64_t submit_started = SDL_GetTicksNS();
    uint64_t perf_submit_started = sr_perf_now_ns();
    if (vkQueueSubmit(s_queue, 1, &si, r->fence) != VK_SUCCESS) {
        t->layout = old_layout;
        return 0;
    }
    if (perf_submit_started) sr_perf_vulkan_submit(perf_submit_started);
    replay_note_submit(SR_PERF_GE_TARGET_READBACK_TRANSITION,
                       SDL_GetTicksNS() - submit_started);
    if (sr_perf_enabled) sr_perf_ge_submit(SR_PERF_GE_TARGET_READBACK_TRANSITION);
    r->target = t;
    r->image = t->img;
    r->gen = t->render_gen;
    r->fba = t->fba; r->stride = t->stride; r->fmt = t->fmt;
    r->commit = copy;
    r->pending = 1;
    return 1;
}

/* Fill the target's color image from guest VRAM (creation / CPU-dirty reacquire).
 * At scale > 1 each guest pixel is replicated into its scale x scale device cell. */
static int target_upload(Target *t) {
    GeGpuFbDescriptor desc = target_descriptor(t->fba, t->stride, t->fmt);
    GeGpuFbSpan guest_span;
    const char *why = NULL;
    if (!gegpu_validate_guest_fb_descriptor(&desc, &guest_span, &why)) {
        fprintf(stderr, "target_upload: invalid target fba=0x%08x stride=%u fmt=%u: %s\n",
                t->fba, t->stride, t->fmt, why ? why : "invalid descriptor");
        return 0;
    }
    uint32_t base = guest_span.base;
    if (!sr_guest_span_readable_current(base, guest_span.total_bytes)) return 0;
    /* CPU VRAM is authoritative on this path.  Never let an older asynchronous copy
     * overwrite it after the upload has begun. */
    readback_discard_target(t);
    VkDeviceSize upload_bytes = (VkDeviceSize)SCL_W * SCL_H * 4u;
    UploadReservation upload;
    if (!upload_reserve(upload_bytes, &upload)) return 0;
    uint32_t *row = s_pxscratch;               /* one guest row of decode scratch */
    uint32_t *dst = (uint32_t *)upload.map;
    uint32_t drow = SCL_W;
    for (uint32_t y = 0; y < FB_H; y++) {
        if (t->fmt == 3) {
            memcpy(row, (const uint32_t *)SR_HOST(base + y * guest_span.row_pitch),
                   t->stride * 4);
        } else {
            const uint16_t *src = (const uint16_t *)SR_HOST(
                base + y * guest_span.row_pitch);
            for (uint32_t x = 0; x < t->stride; x++) row[x] = fb_unpack(src[x], t->fmt);
        }
        for (uint32_t x = t->stride; x < FB_W; x++) row[x] = 0;
        const uint32_t *wide = row;
        if (s_scale > 1) {
            for (uint32_t x = 0; x < FB_W; x++)
                for (int i = 0; i < s_scale; i++) s_pxwide[x * s_scale + i] = row[x];
            wide = s_pxwide;
        }
        uint32_t *dl = dst + (uint32_t)(y * s_scale) * drow;
        for (int j = 0; j < s_scale; j++) memcpy(dl + (uint32_t)j * drow, wide, drow * 4);
    }
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
    c.bufferOffset = upload.offset;
    vkCmdCopyBufferToImage(s_cmd, upload.buffer, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!upload_finish(&upload, SR_PERF_GE_TARGET_UPLOAD)) return 0;
    t->gpu_valid = 1;
    t->cpu_n = 0;                          /* the whole image now holds guest VRAM */
    t->render_gen++;                       /* image content changed (snapshot reuse check) */
    t->clean_gen = t->render_gen;          /* VRAM is the source: in sync by definition */
    s_cnt_upload++;
    return 1;
}

typedef struct DirtyEdgePixel {
    uint32_t x, y;
    uint32_t rgba;
} DirtyEdgePixel;

/* A byte-granular CPU write can cover only part of a packed framebuffer pixel. Guest
 * VRAM cannot supply the untouched bytes in that pixel when the target is GPU-newer, so
 * read back only the one or two boundary pixels. The copy is target-scoped and ordered
 * on the GE queue; ordinary pixel-aligned writes never take this wait. */
static int target_read_dirty_edges(Target *t, DirtyEdgePixel *edges, uint32_t count) {
    if (!count) return 1;
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy copies[2] = {{0}};
    for (uint32_t i = 0; i < count; i++) {
        copies[i].bufferOffset = (VkDeviceSize)i * sizeof(uint32_t);
        copies[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copies[i].imageSubresource.layerCount = 1;
        copies[i].imageOffset.x = (int32_t)(edges[i].x * (uint32_t)s_scale);
        copies[i].imageOffset.y = (int32_t)(edges[i].y * (uint32_t)s_scale);
        copies[i].imageExtent.width = 1;
        copies[i].imageExtent.height = 1;
        copies[i].imageExtent.depth = 1;
    }
    vkCmdCopyImageToBuffer(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s_xfer, count, copies);
    if (!cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION)) return 0;
    for (uint32_t i = 0; i < count; i++)
        edges[i].rgba = ((const uint32_t *)s_xfer_map)[i];
    return 1;
}

static uint32_t target_dirty_edge_rgba(const DirtyEdgePixel *edges, uint32_t count,
                                       uint32_t x, uint32_t y) {
    for (uint32_t i = 0; i < count; i++)
        if (edges[i].x == x && edges[i].y == y) return edges[i].rgba;
    return 0;
}

/* Patch the post-write guest bytes into an overlapping resident target. Pixels outside
 * the dirty byte span remain in the image, so GPU generation G1 survives; covered bytes
 * are decoded from guest generation G2. At scale > 1, each patched guest pixel updates
 * its complete scale x scale device cell, matching target_upload(). */
static int target_patch_vram_dirty(Target *t, uint32_t dirty_addr, uint32_t dirty_bytes) {
    GeGpuFbDescriptor desc = target_descriptor(t->fba, t->stride, t->fmt);
    GeGpuFbSpan guest_span;
    const char *why = NULL;
    if (!gegpu_validate_guest_fb_descriptor(&desc, &guest_span, &why)) return 0;
    uint32_t bpp = t->fmt == 3 ? 4u : 2u;
    uint64_t target0 = t->fba;
    uint64_t target1 = target0 + (uint64_t)t->stride * FB_H * bpp;
    uint64_t dirty0 = dirty_addr;
    uint64_t dirty1 = dirty0 + dirty_bytes;
    uint64_t overlap0 = dirty0 > target0 ? dirty0 : target0;
    uint64_t overlap1 = dirty1 < target1 ? dirty1 : target1;
    if (overlap0 >= overlap1) return 1;

    readback_discard_target(t);
    int was_clean = t->clean_gen == t->render_gen;
    uint32_t row_bytes = t->stride * bpp;
    uint32_t row_x0[FB_H], row_x1[FB_H];
    uint8_t row_used[FB_H] = {0};
    VkDeviceSize upload_bytes = 0;
    uint32_t copy_count = 0;
    for (uint32_t y = 0; y < FB_H; y++) {
        uint64_t row0 = target0 + (uint64_t)y * row_bytes;
        uint64_t row1 = row0 + row_bytes;
        uint64_t part0 = overlap0 > row0 ? overlap0 : row0;
        uint64_t part1 = overlap1 < row1 ? overlap1 : row1;
        if (part0 >= part1) continue;
        uint32_t byte0 = (uint32_t)(part0 - row0);
        uint32_t byte1 = (uint32_t)(part1 - row0);
        row_x0[y] = byte0 / bpp;
        row_x1[y] = (byte1 + bpp - 1u) / bpp;
        row_used[y] = 1;
        VkDeviceSize width = (VkDeviceSize)(row_x1[y] - row_x0[y]) * (uint32_t)s_scale;
        upload_bytes += width * (uint32_t)s_scale * sizeof(uint32_t);
        copy_count++;
    }
    if (!copy_count || upload_bytes == 0) return 1;

    DirtyEdgePixel edges[2];
    uint32_t edge_count = 0;
    if (!was_clean) {
        uint64_t relative0 = overlap0 - target0;
        uint64_t relative1 = overlap1 - target0;
        if (relative0 % bpp) {
            uint64_t pixel = relative0 / bpp;
            edges[edge_count].y = (uint32_t)(pixel / t->stride);
            edges[edge_count].x = (uint32_t)(pixel % t->stride);
            edge_count++;
        }
        if (relative1 % bpp) {
            uint64_t pixel = (relative1 - 1u) / bpp;
            uint32_t y = (uint32_t)(pixel / t->stride);
            uint32_t x = (uint32_t)(pixel % t->stride);
            if (!edge_count || edges[0].x != x || edges[0].y != y) {
                edges[edge_count].x = x;
                edges[edge_count].y = y;
                edge_count++;
            }
        }
        if (!target_read_dirty_edges(t, edges, edge_count)) return 0;
    }

    UploadReservation upload;
    if (!upload_reserve(upload_bytes, &upload)) return 0;
    VkBufferImageCopy copies[FB_H] = {{0}};
    VkDeviceSize used = 0;
    uint32_t ci = 0;
    uint32_t base = guest_span.base;
    for (uint32_t y = 0; y < FB_H; y++) {
        if (!row_used[y]) continue;
        uint32_t x0 = row_x0[y], x1 = row_x1[y];
        uint32_t scaled_width = (x1 - x0) * (uint32_t)s_scale;
        uint32_t *dst = (uint32_t *)(upload.map + used);
        uint64_t row0 = target0 + (uint64_t)y * row_bytes;
        const uint8_t *guest_row = (const uint8_t *)SR_HOST(
            base + y * guest_span.row_pitch);
        for (uint32_t x = x0; x < x1; x++) {
            uint64_t pixel0 = row0 + (uint64_t)x * bpp;
            uint64_t pixel1 = pixel0 + bpp;
            uint32_t raw;
            if (was_clean || (overlap0 <= pixel0 && pixel1 <= overlap1)) {
                raw = 0;
                memcpy(&raw, guest_row + x * bpp, bpp);
            } else {
                uint32_t rgba = target_dirty_edge_rgba(edges, edge_count, x, y);
                raw = t->fmt == 3 ? rgba : fb_pack(rgba, t->fmt);
                uint64_t byte0 = overlap0 > pixel0 ? overlap0 : pixel0;
                uint64_t byte1 = overlap1 < pixel1 ? overlap1 : pixel1;
                memcpy((uint8_t *)&raw + (size_t)(byte0 - pixel0),
                       guest_row + x * bpp + (size_t)(byte0 - pixel0),
                       (size_t)(byte1 - byte0));
            }
            uint32_t rgba = fb_unpack(raw, t->fmt);
            uint32_t dx = (x - x0) * (uint32_t)s_scale;
            for (int sx = 0; sx < s_scale; sx++) s_pxwide[dx + (uint32_t)sx] = rgba;
        }
        for (int sy = 0; sy < s_scale; sy++)
            memcpy(dst + (uint32_t)sy * scaled_width, s_pxwide,
                   (size_t)scaled_width * sizeof(uint32_t));

        copies[ci].bufferOffset = upload.offset + used;
        copies[ci].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copies[ci].imageSubresource.layerCount = 1;
        copies[ci].imageOffset.x = (int32_t)(x0 * (uint32_t)s_scale);
        copies[ci].imageOffset.y = (int32_t)(y * (uint32_t)s_scale);
        copies[ci].imageExtent.width = scaled_width;
        copies[ci].imageExtent.height = (uint32_t)s_scale;
        copies[ci].imageExtent.depth = 1;
        used += (VkDeviceSize)scaled_width * (uint32_t)s_scale * sizeof(uint32_t);
        ci++;
    }

    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    vkCmdCopyBufferToImage(s_cmd, upload.buffer, t->img,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ci, copies);
    to_layout(s_cmd, t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!upload_finish(&upload, SR_PERF_GE_TARGET_UPLOAD)) return 0;

    t->gpu_valid = 1;
    t->render_gen++;
    if (was_clean || (overlap0 == target0 && overlap1 == target1))
        t->clean_gen = t->render_gen;
    s_cnt_upload++;
    return 1;
}

/* Apply the CPU writes held for `t` to its image, in guest order relative to the GE: no GE
 * operation has used the target since they were held, so the image still holds what it held
 * when each write happened, and guest VRAM holds the bytes the writes left. Batches already
 * queued for the current target were drawn before the writes, so they go first. */
static int target_sync_cpu_writes(Target *t) {
    if (!t || !t->cpu_n) return 1;
    if (t == s_cur && s_nbatch && !submit_pending()) {
        fprintf(stderr, "gegpu: failed to submit target before CPU VRAM patch\n");
        t->gpu_valid = 0;
        t->cpu_n = 0;
        return 0;
    }
    const int n = t->cpu_n;
    t->cpu_n = 0;
    s_replay_stats.cpu_write_syncs++;
    for (int i = 0; i < n; i++) {
        if (!t->gpu_valid) break;
        if (!target_patch_vram_dirty(t, t->cpu_lo[i], t->cpu_hi[i] - t->cpu_lo[i])) {
            fprintf(stderr, "gegpu: failed to preserve target across CPU VRAM patch\n");
            t->gpu_valid = 0;
            return 0;
        }
    }
    return 1;
}

/* Hold one CPU write [lo, hi) for `t` (VRAM offsets), merging it with every span it overlaps
 * or touches. A write needing a span beyond TARGET_CPU_SPANS applies the held ones first. */
static void target_note_cpu_write(Target *t, uint32_t lo, uint32_t hi) {
    s_replay_stats.cpu_write_notes++;
    int i = 0;
    while (i < t->cpu_n && t->cpu_hi[i] < lo) i++;
    int j = i;
    while (j < t->cpu_n && t->cpu_lo[j] <= hi) {
        if (t->cpu_lo[j] < lo) lo = t->cpu_lo[j];
        if (t->cpu_hi[j] > hi) hi = t->cpu_hi[j];
        j++;
    }
    if (j == i && t->cpu_n == TARGET_CPU_SPANS) {
        if (!target_sync_cpu_writes(t) || !t->gpu_valid) return;
        i = j = 0;
    }
    const int keep = t->cpu_n - j;
    memmove(&t->cpu_lo[i + 1], &t->cpu_lo[j], (size_t)keep * sizeof(t->cpu_lo[0]));
    memmove(&t->cpu_hi[i + 1], &t->cpu_hi[j], (size_t)keep * sizeof(t->cpu_hi[0]));
    t->cpu_lo[i] = lo;
    t->cpu_hi[i] = hi;
    t->cpu_n = i + 1 + keep;
}

static void target_destroy(Target *t) {
    if (!t->used) return;
    (void)cmd_drain(GEGPU_BOUNDARY_LIFETIME);
    /* Retire only submissions that actually reference this image. */
    (void)readback_wait_target(t, 0);
    (void)sdl3vk_wait_image((void *)t->img);
    if (s_snap_src == t) s_snap_src = NULL;
    if (t->fb) vkDestroyFramebuffer(s_dev, t->fb, NULL);
    for (unsigned i = 0; i < TEX_FILTER_VARIANTS; i++)
        if (t->set[i]) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &t->set[i]);
    if (t->view) vkDestroyImageView(s_dev, t->view, NULL);
    if (t->img) vkDestroyImage(s_dev, t->img, NULL);
    if (t->mem) vkFreeMemory(s_dev, t->mem, NULL);
    memset(t, 0, sizeof(*t));
}

static Target *target_find_by_fba(uint32_t fba) {
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used && s_tgts[i].fba == fba) return &s_tgts[i];
    return NULL;
}

static Target *target_slot_acquire(void) {
    for (int i = 0; i < MAX_TGT; i++)
        if (!s_tgts[i].used) return &s_tgts[i];

    Target *old = NULL;
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used && (!old || s_tgts[i].lru < old->lru) && &s_tgts[i] != s_cur)
            old = &s_tgts[i];
    if (!old && s_cur) {
        submit_pending();
        old = s_cur;
        s_cur = NULL;
    }
    if (!old) return NULL;
    target_readback(old);
    target_destroy(old);
    return old;
}

static Target *target_color_acquire(uint32_t fba, uint32_t stride, uint32_t fmt, int seed_from_vram) {
    if (stride == 0) stride = 512;
    /* A stride beyond the image width cannot be represented by these fixed-size
     * targets: reject (callers fall back to the ge.c software path / CPU present)
     * rather than clamp, which silently corrupted row addressing. */
    if (stride > FB_W) return NULL;
    fmt &= 3;
    GeGpuFbDescriptor desc = target_descriptor(fba, stride, fmt);
    if (!gegpu_validate_guest_fb_descriptor(&desc, NULL, NULL)) return NULL;

    Target *t = target_find_by_fba(fba);
    if (t && (t->stride != stride || t->fmt != fmt)) {
        target_readback(t);
        if (t == s_cur) { submit_pending(); s_cur = NULL; }
        target_destroy(t);
        t = NULL;
    }

    if (!t) {
        t = target_slot_acquire();
        if (!t) return NULL;
        memset(t, 0, sizeof(*t));
        t->fba = fba; t->stride = stride; t->fmt = fmt; t->used = 1;
        t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!make_image(SCL_W, SCL_H, VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        &t->img, &t->mem)) { target_destroy(t); return NULL; }
        if (!make_view(t->img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &t->view)) {
            target_destroy(t); return NULL;
        }
        for (unsigned i = 0; i < TEX_FILTER_VARIANTS; i++) {
            t->set[i] = make_descriptor(t->view, s_smp[i], s_dpool_fix);
            if (!t->set[i]) { target_destroy(t); return NULL; }
        }
    }

    if (seed_from_vram && !t->gpu_valid) {
        if (!target_upload(t)) return NULL;
    }
    if (!target_sync_cpu_writes(t)) return NULL;
    t->lru = s_lru++;
    return t;
}

static int depth_from_cpu(DepthEnt *d) {
    VkDeviceSize upload_bytes = (VkDeviceSize)SCL_W * SCL_H * 2u;
    UploadReservation upload;
    if (!upload_reserve(upload_bytes, &upload)) return 0;
    uint32_t source_stride = d->zstride;
    uint16_t *dst = (uint16_t *)upload.map;
    uint32_t drow = SCL_W;
    for (uint32_t y = 0; y < FB_H; y++) {
        const uint16_t *src = s_zbuf + y * source_stride;
        const uint16_t *wide = src;
        if (s_scale > 1) {   /* replicate from cached memory, never the mapped upload (s_zwide) */
            for (uint32_t x = 0; x < FB_W; x++)
                for (int i = 0; i < s_scale; i++) s_zwide[x * s_scale + i] = src[x];
            wide = s_zwide;
        }
        uint16_t *dl = dst + (uint32_t)(y * s_scale) * drow;
        for (int j = 0; j < s_scale; j++) memcpy(dl + (uint32_t)j * drow, wide, drow * 2);
    }
    to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
    c.bufferOffset = upload.offset;
    vkCmdCopyBufferToImage(s_cmd, upload.buffer, d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    if (!upload_finish(&upload, SR_PERF_GE_DEPTH_UPLOAD)) return 0;
    d->cpu_dirty = 0;
    d->guest_dirty = 0;
    return 1;
}

static int depth_to_cpu(DepthEnt *d) {
    if (!d || d->cpu_dirty) return 1;
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, d->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_xfer, 1, &c);
    to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    if (!cmd_submit_wait(SR_PERF_GE_DEPTH_READBACK)) return 0;
    const uint16_t *src = (const uint16_t *)s_xfer_map;
    for (uint32_t y = 0; y < FB_H; y++)
        for (uint32_t x = 0; x < FB_W; x++)
            s_zbuf[y * d->zstride + x] =
                src[(uint32_t)(y * s_scale) * SCL_W + x * s_scale];
    d->cpu_dirty = 1;
    d->guest_dirty = 0;
    ge_zbuf_note_external_write();
    return 1;
}

static int depth_span_overlaps(const DepthEnt *d, uint32_t addr, uint32_t bytes) {
    if (!d || !bytes) return 0;
    uint32_t low, high;
    if (!ge_zbuf_guest_span_bounds(0x04000000u + d->zba, d->zstride,
                                   &low, &high)) return 0;
    uint64_t dirty0 = SR_PHYS(addr);
    uint64_t dirty1 = dirty0 + bytes;
    return dirty0 < high && low < dirty1;
}

static int depth_readback_to_guest(DepthEnt *d) {
    if (!d || !d->guest_dirty) return 1;
    if ((d->zba & 0x40u) != 0u) {
        fprintf(stderr,
                "SEMANTIC_BOUNDARY: VRAM_DEPTH_BIT6_UNRESOLVED; depth buffers with bit 6 set are not supported yet: the PSP wrote no depth for zbp 0x0cc040 (2026-10-21).\n");
        return 0;
    }
    uint32_t depth_first, depth_end;
    if (d->zba >= GEGPU_VRAM_BYTES ||
        !ge_zbuf_guest_span_bounds(0x04000000u + d->zba, d->zstride,
                                   &depth_first, &depth_end) ||
        depth_first < 0x04000000u || depth_end > 0x04200000u)
        return 0;
    if (s_nbatch && !submit_pending()) return 0;
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, d->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_xfer, 1, &c);
    to_layout(s_cmd, d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
              VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    if (!cmd_submit_wait(SR_PERF_GE_DEPTH_READBACK)) return 0;

    const uint16_t *src = (const uint16_t *)s_xfer_map;
    uint32_t width = d->zstride < FB_W ? d->zstride : FB_W;
    for (uint32_t y = 0; y < FB_H; y++) {
        for (uint32_t x = 0; x < width; x++) {
            uint32_t linear = (y * d->zstride + x) * 2u;
            uint32_t addr = 0x04000000u + ((d->zba + linear) ^ 0x2040u);
            uint16_t value = src[(uint32_t)(y * s_scale) * SCL_W + x * (uint32_t)s_scale];
            if (!sr_inrange_n(addr, 2u)) return 0;
            memcpy(SR_HOST_LINEAR(addr), &value, sizeof value);
        }
    }
    d->guest_dirty = 0;
    ge_zbuf_invalidate_guest_span(depth_first, depth_end - depth_first);
    if (sr_perf_enabled) sr_perf_vulkan_readback(FB_W * FB_H * 2u);
    return 1;
}

static DepthEnt *depth_acquire(uint32_t zba, uint32_t zstride) {
    for (int i = 0; i < MAX_DEP; i++)
        if (s_deps[i].used && s_deps[i].zba == zba && s_deps[i].zstride == zstride) {
            if (s_deps[i].cpu_dirty && !depth_from_cpu(&s_deps[i])) return NULL;
            return &s_deps[i];
        }
    DepthEnt *d = NULL;
    for (int i = 0; i < MAX_DEP; i++) if (!s_deps[i].used) { d = &s_deps[i]; break; }
    if (!d) {   /* evict slot 0 arbitrarily (depth contents are transient frame data) */
        /* Render submissions are fenced by cmd_submit_wait; flush only batches that
         * can still reference this depth attachment. */
        if (!submit_pending()) return NULL;
        if (!cmd_drain(GEGPU_BOUNDARY_LIFETIME)) return NULL;
        d = &s_deps[0];
        vkDestroyImageView(s_dev, d->view, NULL);
        vkDestroyImage(s_dev, d->img, NULL);
        vkFreeMemory(s_dev, d->mem, NULL);
        /* targets holding this depth must drop their framebuffers */
        for (int i = 0; i < MAX_TGT; i++)
            if (s_tgts[i].used && s_tgts[i].dep == d) {
                vkDestroyFramebuffer(s_dev, s_tgts[i].fb, NULL);
                s_tgts[i].fb = VK_NULL_HANDLE; s_tgts[i].dep = NULL;
            }
        memset(d, 0, sizeof(*d));
    }
    if (!make_image(SCL_W, SCL_H, VK_FORMAT_D16_UNORM,
                    VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                    VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    &d->img, &d->mem)) return NULL;
    if (!make_view(d->img, VK_FORMAT_D16_UNORM, VK_IMAGE_ASPECT_DEPTH_BIT, &d->view)) return NULL;
    d->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    d->zba = zba; d->zstride = zstride; d->used = 1;
    if (__builtin_expect(g_sr_vram_read_pending, 0)) {
        uint32_t first, end;
        if (!ge_zbuf_guest_span_bounds(0x04000000u + zba, zstride,
                                       &first, &end)) return NULL;
        sr_ge_vram_read_prepare(first, end - first);
        if (!ge_zbuf_prepare_current()) return NULL;
        s_zbuf = ge_zbuf_ptr();
        if (!s_zbuf) return NULL;
    }
    /* initialize from the (CPU) software z-buffer once — games clear depth before use,
     * this just avoids garbage on the very first frame */
    if (!depth_from_cpu(d)) return NULL;
    return d;
}

static Target *target_acquire(void) {
    if (!is_vram(s_ge->fbp)) return NULL;    /* render-to-RAM: software path owns it */
    uint32_t fba = vram_off(s_ge->fbp);
    uint32_t stride = s_ge->fbw ? s_ge->fbw : 512;
    uint32_t fmt = s_ge->fbfmt & 3;
    uint32_t zba = vram_off(s_ge->zbp);      /* depth is VRAM-only on the GE */
    uint32_t zstride = s_ge->zbw ? s_ge->zbw : stride;
    if (zstride > 512u || !ge_zbuf_prepare_current()) return NULL;
    s_zbuf = ge_zbuf_ptr();
    if (!s_zbuf) return NULL;

    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t) return NULL;

    DepthEnt *d = depth_acquire(zba, zstride);
    if (!d) return NULL;
    if (t->dep != d || !t->fb) {
        if (t->fb) {
            if (!cmd_drain(GEGPU_BOUNDARY_LIFETIME)) return NULL;
            vkDestroyFramebuffer(s_dev, t->fb, NULL);
        }
        VkImageView views[2] = { t->view, d->view };
        VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        fbc.renderPass = s_rp; fbc.attachmentCount = 2; fbc.pAttachments = views;
        fbc.width = SCL_W; fbc.height = SCL_H; fbc.layers = 1;
        if (vkCreateFramebuffer(s_dev, &fbc, NULL, &t->fb) != VK_SUCCESS) return NULL;
        t->dep = d;
    }
    t->lru = s_lru++;
    return t;
}

/* Ensure s_cur points at the GE's current target, submitting pending work on a switch. */
static int begin_target(void) {
    if (s_cpu_profile) s_cpu_profile_stats.target_calls++;
    if (!is_vram(s_ge->fbp)) return 0;       /* render-to-RAM: software path owns it */
    uint32_t fba = vram_off(s_ge->fbp);
    uint32_t stride = s_ge->fbw ? s_ge->fbw : 512;
    if (s_cur && s_cur->used && s_cur->fba == fba &&
        s_cur->fmt == (s_ge->fbfmt & 3) && s_cur->stride == stride && s_cur->gpu_valid) {
        if (!target_sync_cpu_writes(s_cur)) return 0;
        if (!ge_zbuf_prepare_current()) return 0;
        s_zbuf = ge_zbuf_ptr();
        if (!s_zbuf || (s_cur->dep && s_cur->dep->cpu_dirty &&
                        !depth_from_cpu(s_cur->dep))) return 0;
        s_cur->lru = s_lru++;
        if (s_cpu_profile) s_cpu_profile_stats.target_fast_hits++;
        return 1;
    }
    if (s_cpu_profile) s_cpu_profile_stats.target_acquires++;
    if (s_nbatch) submit_pending();
    s_cur = target_acquire();
    return s_cur != NULL;
}

/* ---- texture cache (RAM textures, decoded by ge.c's reference sampler) ------------------- */

/* Source bytes of one texture level (level 0, or a mip level: same format and swizzle). */
static uint32_t tex_span_size(uint32_t bufw, int tw, int th) {
    uint32_t bpp_num;
    switch (s_ge->tex_fmt) {
        case 3: case 7: bpp_num = 32; break;   /* 8888 / CLUT32 */
        case 4: case 8: bpp_num = 4;  break;   /* CLUT4 / DXT1 */
        case 5: case 9: case 10: bpp_num = 8; break; /* CLUT8 / DXT3 / DXT5 */
        default: bpp_num = 16; break;
    }
    uint32_t stride = bufw ? bufw : (uint32_t)tw;
    uint32_t w = tw > 0 ? (uint32_t)tw : 1u;
    uint32_t h = th > 0 ? (uint32_t)th : 1u;
    uint64_t bytes;
    if (s_ge->tex_fmt >= 8 && s_ge->tex_fmt <= 10) {
        uint32_t blocks_per_row = stride / 4u ? stride / 4u : 1u;
        uint64_t last_block = ((uint64_t)(h - 1u) / 4u) * blocks_per_row +
                              ((uint64_t)(w - 1u) / 4u);
        bytes = (last_block + 1u) * (s_ge->tex_fmt == 8 ? 8u : 16u);
    } else if (s_ge->tex_swizzle) {
        /* Match ge.c::texel_offset for the last accessed texel. Swizzled storage is
         * padded in 8-row tiles, so stride*height can undercount short textures. */
        uint32_t texels_per_tile = 32u / bpp_num;
        uint32_t u = w - 1u, v = h - 1u;
        uint32_t tile_u = u / texels_per_tile;
        uint64_t tile_idx = (uint64_t)(v % 8u) * 4u
            + (uint64_t)(v / 8u) * ((stride * bpp_num / 32u) * 8u)
            + (tile_u % 4u) + (uint64_t)(tile_u / 4u) * 32u;
        bytes = tile_idx * 4u + (((u % texels_per_tile) * bpp_num) >> 3) +
                (bpp_num < 8u ? 1u : bpp_num >> 3);
    } else {
        /* Linear rows are whole 16-byte lines, as ge.c samples them (ge_tex_row_bytes). */
        bytes = (uint64_t)ge_tex_row_bytes(stride, bpp_num) * h;
    }
    if (bytes > 4u << 20) bytes = 4u << 20;
    return (uint32_t)bytes;
}

static uint32_t tex_source_size(void) {
    return tex_span_size(s_ge->tex_bufw, s_ge->tex_w, s_ge->tex_h);
}

/* Source bytes of mip level `level` (1-7) of the current texture. */
static uint32_t tex_level_source_size(uint32_t level) {
    return tex_span_size(s_ge->tex_mip_bufw[level - 1u], 1 << s_ge->tex_mip_wlog2[level - 1u],
                         1 << s_ge->tex_mip_hlog2[level - 1u]);
}

static const uint8_t *tex_source_ptr(uint32_t addr, uint32_t bytes) {
    /* A source through a swizzled VRAM mirror has no single host pointer (#1049). It is
     * decoded through ge_decode_tex_rgba's piecewise copy, and its cache entry is kept
     * current by hook_vram_dirty, like any source this function cannot read. That is an
     * answered shape, not a boundary, so it is not reported as one. */
    if (sr_guest_span_hits_swizzled_vram_mirror(addr, bytes)) return NULL;
    if (!bytes || !sr_guest_span_readable_current(addr, bytes)) return NULL;
    /* SR_HOST maps VRAM mirrors, but a direct memcmp/copy may not cross the physical
     * 2 MiB wrap. Fall back to the exact decoder on that rare shape. */
    if (is_vram(addr) && (uint64_t)vram_off(addr) + bytes > 0x200000u) return NULL;
    return (const uint8_t *)SR_HOST(addr);
}

static int tex_shadow_matches(TexEnt *e) {
    if (!s_tex_shadow_enabled || !e->shadow || e->shadow_bytes != e->bytes) return 0;
    const uint8_t *src = tex_source_ptr(e->addr, e->bytes);
    if (!src) return 0;
    uint64_t started = cpu_profile_now();
    s_cnt_shadow_checks++;
    if (s_cpu_profile) s_cpu_profile_stats.texture_shadow_checks++;
    int match = memcmp(e->shadow, src, e->bytes) == 0;
    uint64_t compared = e->bytes;
    if (match && s_ge->tex_fmt >= 4 && s_ge->tex_fmt <= 7) {
        compared += sizeof(e->shadow_clut);
        match = e->shadow_clut_valid && e->shadow_clut_fmt == s_ge->clut_fmt &&
                memcmp(e->shadow_clut, s_ge->clutram, sizeof(e->shadow_clut)) == 0;
    }
    s_cnt_shadow_bytes += compared;
    if (match) s_cnt_shadow_hits++; else s_cnt_shadow_misses++;
    if (s_cpu_profile) {
        s_cpu_profile_stats.texture_shadow_bytes += e->bytes;
        if (match) s_cpu_profile_stats.texture_shadow_hits++;
        cpu_profile_add(GEGPU_CPU_TEXTURE_SHADOW, started);
    }
    return match;
}

static void tex_shadow_store(TexEnt *e) {
    /* A shadow holds level 0's bytes only, so it cannot prove a mip chain unchanged: an
     * invalidated chain is decoded again (tex_shadow_matches then finds no shadow). */
    if (!s_tex_shadow_enabled || !e->bytes || e->mip.max_level) return;
    const uint8_t *src = tex_source_ptr(e->addr, e->bytes);
    if (!src) return;
    if (!e->shadow || e->shadow_bytes != e->bytes) {
        size_t retained = s_tex_shadow_bytes - e->shadow_bytes;
        if (retained + e->bytes > TEX_SHADOW_MAX_BYTES) return;
        uint64_t heap_started = cpu_profile_now();
        uint8_t *replacement = (uint8_t *)malloc(e->bytes);
        if (s_cpu_profile) cpu_profile_add(GEGPU_CPU_HEAP, heap_started);
        if (!replacement) return;
        free(e->shadow);
        s_tex_shadow_bytes = retained + e->bytes;
        e->shadow = replacement;
        e->shadow_bytes = e->bytes;
    }
    uint64_t memcpy_started = cpu_profile_now();
    memcpy(e->shadow, src, e->bytes);
    if (s_ge->tex_fmt >= 4 && s_ge->tex_fmt <= 7) {
        memcpy(e->shadow_clut, s_ge->clutram, sizeof(e->shadow_clut));
        e->shadow_clut_fmt = s_ge->clut_fmt;
        e->shadow_clut_valid = 1;
    } else {
        e->shadow_clut_valid = 0;
    }
    if (s_cpu_profile) {
        cpu_profile_add(GEGPU_CPU_MEMCPY, memcpy_started);
        s_cpu_profile_stats.memcpy_bytes += e->bytes +
            (e->shadow_clut_valid ? sizeof(e->shadow_clut) : 0u);
    }
}

static void tex_shadow_release(TexEnt *e) {
    if (!e->shadow) return;
    uint64_t heap_started = cpu_profile_now();
    free(e->shadow);
    if (s_cpu_profile) cpu_profile_add(GEGPU_CPU_HEAP, heap_started);
    s_tex_shadow_bytes -= e->shadow_bytes;
    e->shadow = NULL;
    e->shadow_bytes = 0;
    e->shadow_clut_valid = 0;
}

static uint64_t fnv64(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h;
}

/* The FNV-1a step taken a 64-bit word at a time over `n` bytes, a multiple of 8: an eighth
 * of fnv64's dependent multiplies. Each step is a bijection of the running hash and, for a
 * given hash, of the word, so a change confined to any one word always changes the result. */
static uint64_t fnv64_words(uint64_t h, const void *data, size_t n) {
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i + 8u <= n; i += 8u) {
        uint64_t w;
        memcpy(&w, p + i, 8u);
        h ^= w;
        h *= 1099511628211ull;
    }
    return h;
}

static uint64_t tex_hash(void) {
    uint64_t profile_started = cpu_profile_now();
    uint32_t source_bytes = tex_source_size();
    uint64_t bytes = source_bytes;
    const uint8_t *p = tex_source_ptr(s_ge->tex_addr, source_bytes);
    if (!p) {
        cpu_profile_add(GEGPU_CPU_OBJECT_LOOKUP, profile_started);
        return 0;
    }
    uint64_t h = 1469598103934665603ull;
    h = fnv64_words(h, &bytes, sizeof(bytes));
    uint64_t step = bytes / 64; if (step < 16) step = 16;
    for (uint64_t off = 0; off + 16 <= bytes; off += step) h = fnv64_words(h, p + off, 16);
    if (bytes >= 16) h = fnv64_words(h, p + bytes - 16, 16);
    cpu_profile_add(GEGPU_CPU_OBJECT_LOOKUP, profile_started);
    return h;
}

/* Fold mip levels 1..max_level into a chain's content hash with tex_hash's sampling. A level
 * whose source the hash cannot read (tex_source_ptr) adds its size only; like level 0 it is
 * then kept current by hook_vram_dirty alone. */
static uint64_t tex_mip_hash(uint64_t h, const TexMipKey *mip, const uint32_t *level_bytes) {
    uint64_t profile_started = cpu_profile_now();
    for (uint32_t i = 0; i < mip->max_level; i++) {
        uint64_t bytes = level_bytes[i];
        h = fnv64_words(h, &bytes, sizeof(bytes));
        const uint8_t *p = tex_source_ptr(mip->addr[i], level_bytes[i]);
        if (!p) continue;
        uint64_t step = bytes / 64; if (step < 16) step = 16;
        for (uint64_t off = 0; off + 16 <= bytes; off += step) h = fnv64_words(h, p + off, 16);
        if (bytes >= 16) h = fnv64_words(h, p + bytes - 16, 16);
        else h = fnv64(h, p, (size_t)bytes);
    }
    cpu_profile_add(GEGPU_CPU_OBJECT_LOOKUP, profile_started);
    return h;
}

/* Palette identity for CLUT textures. This must be part of the cache KEY, not just the
 * content hash: particle systems (explosions, fire) draw one greyscale texture through
 * many palettes in a single frame. Keyed without the CLUT those draws alias one cache
 * entry and ping-pong it — a full CPU decode + GPU upload + queue wait on EVERY draw. */
static uint64_t clut_hash(void) {
    uint64_t profile_started = cpu_profile_now();
    uint64_t h = 1469598103934665603ull;
    h = fnv64_words(h, s_ge->clutram, sizeof(s_ge->clutram));
    h = fnv64(h, &s_ge->clut_fmt, sizeof(s_ge->clut_fmt));
    cpu_profile_add(GEGPU_CPU_OBJECT_LOOKUP, profile_started);
    return h;
}

static uint64_t s_texlru = 1;

/* Evict the least-recently-used quarter of the cache (the working set survives; the
 * old full-clear rebuilt EVERY texture every frame once a scene exceeded the cap). */
static void tex_evict_lru(void) {
    /* The caller submits batches before eviction.  Texture uploads and render submits
     * use the scoped GE fence, so there is no device-wide lifetime dependency here. */
    if (!cmd_drain(GEGPU_BOUNDARY_LIFETIME)) return;
    int goal = s_tex_n - MAX_TEX / 4;
    while (s_tex_n > goal) {
        int v = 0;
        for (int i = 1; i < s_tex_n; i++)
            if (s_tex[i].lru < s_tex[v].lru) v = i;
        TexEnt *e = &s_tex[v];
        tex_shadow_release(e);
        vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set);
        vkDestroySampler(s_dev, e->smp, NULL);
        if (e->set_af) vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set_af);
        if (e->smp_af) vkDestroySampler(s_dev, e->smp_af, NULL);
        vkDestroyImageView(s_dev, e->view, NULL);
        vkDestroyImage(s_dev, e->img, NULL);
        vkFreeMemory(s_dev, e->mem, NULL);
        *e = s_tex[--s_tex_n];
    }
}

/* Copy one decoded level into mip level `level` of `img`. `lay` tracks the image layout
 * across the levels of one chain: level 0 moves every level to TRANSFER_DST (the old
 * contents are discarded) and the last level moves them all to SHADER_READ_ONLY. Each level
 * has its own staging reservation; a reservation that submits the open chain between two
 * levels leaves the barriers and copies in submission order on the one GE queue. */
static int tex_upload_level(VkImage img, VkImageLayout *lay, uint32_t level, int last,
                            const uint32_t *px, int w, int h) {
    VkDeviceSize upload_bytes = (VkDeviceSize)(uint32_t)w * (uint32_t)h * 4u;
    UploadReservation upload;
    if (!upload_reserve(upload_bytes, &upload)) return 0;
    uint64_t memcpy_started = cpu_profile_now();
    memcpy(upload.map, px, (size_t)upload_bytes);
    if (s_cpu_profile) {
        cpu_profile_add(GEGPU_CPU_MEMCPY, memcpy_started);
        s_cpu_profile_stats.memcpy_bytes += (uint64_t)upload_bytes;
    }
    if (level == 0u)
        to_layout(s_cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, lay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.mipLevel = level;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = (uint32_t)w; bic.imageExtent.height = (uint32_t)h; bic.imageExtent.depth = 1;
    bic.bufferOffset = upload.offset;
    vkCmdCopyBufferToImage(s_cmd, upload.buffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    if (last)
        to_layout(s_cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, lay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return upload_finish(&upload, SR_PERF_GE_TEXTURE_UPLOAD);
}

/* Decode the current texture's levels 0..levels-1 through ge.c's sampler and upload them.
 * Chain level L is max(1, w >> L) x max(1, h >> L), which tex_mip_chain_supported() checked
 * is the level's own TEXSIZE. */
static int tex_decode_upload(VkImage img, int w, int h, uint32_t levels) {
    VkImageLayout lay = VK_IMAGE_LAYOUT_UNDEFINED;
    if (sr_perf_enabled) sr_perf_texture_cache(0);
    for (uint32_t level = 0; level < levels; level++) {
        int lw = w >> level, lh = h >> level;
        if (lw < 1) lw = 1;
        if (lh < 1) lh = 1;
        uint64_t decode_started = cpu_profile_now();
        uint64_t perf_decode_started = sr_perf_now_ns();
        if (level == 0u) ge_decode_tex_rgba(s_texscratch);
        else ge_decode_tex_level_rgba(level, s_texscratch);
        if (perf_decode_started)
            sr_perf_texture_decode(perf_decode_started,
                                   level ? tex_level_source_size(level) : tex_source_size());
        cpu_profile_add(GEGPU_CPU_TEXTURE_DECODE, decode_started);
        if (!tex_upload_level(img, &lay, level, level + 1u == levels, s_texscratch, lw, lh))
            return 0;
    }
    return 1;
}

/* Whether TEXLEVEL mode `mode` picks the level of detail per fragment (automatic 0, slope 2):
 * psp.frag computes it (F_LODFRAG), and the sampler spans every level (#1373). */
static int tex_lod_per_fragment(uint32_t mode) {
    return mode == 0u || mode == 2u;
}

/* Sampler level of detail of one cache entry. With a mip chain (#1373) in a per-fragment
 * mode the shader passes the level of detail (textureLod), so the sampler spans levels 0 to
 * the highest. In constant mode the level of detail is fixed: ge_tex_level_pick's level, plus its blend
 * weight in 1/16 under TEXFILTER bit 1 (VK_SAMPLER_MIPMAP_MODE_LINEAR; the blend may differ
 * from ge.c's truncated sixteenths by one U8 step). Vulkan filters within a level by
 * minFilter above level of detail 0 and by magFilter otherwise, and ge.c shade() picks the
 * minification field for any level of detail above 0: a positive bias that still selects
 * level 0 samples at 0.25, which keeps nearest mipmap mode on level 0 and the minification
 * filter on. */
static void tex_sampler_lod(const TexMipKey *mip, VkSamplerCreateInfo *sci) {
    sci->mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    if (!mip->max_level) {
        /* maxLod=0 forces Vulkan's magnification filter even while minifying. Keep lambda
         * nonzero for minFilter selection while nearest mipmap mode remains at base level 0. */
        sci->maxLod = 0.25f;
        return;
    }
    if (mip->linear) sci->mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    if (tex_lod_per_fragment(mip->mode)) {
        sci->minLod = 0.0f;
        sci->maxLod = (float)mip->max_level;
        return;
    }
    int detail16 = (int)(mip->bias * 16.0f);
    uint32_t level = 0u;
    int frac = 0;
    ge_tex_level_pick(detail16, mip->linear ? 2u : 0u, mip->max_level, &level, &frac);
    float lod = (float)level + (float)frac / 16.0f;
    if (detail16 > 0 && lod == 0.0f) lod = 0.25f;
    sci->minLod = lod;
    sci->maxLod = lod;
}

/* The sampler of a cache entry with these filters, wrap modes and mip state. */
static void tex_sampler_info(int min_linear, int mag_linear, int clamp_u, int clamp_v,
                             const TexMipKey *mip, VkSamplerCreateInfo *sci) {
    memset(sci, 0, sizeof(*sci));
    sci->sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci->magFilter = mag_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci->minFilter = min_linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    tex_sampler_lod(mip, sci);
    sci->addressModeU = clamp_u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci->addressModeV = clamp_v ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci->addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

static int tex_make(int w, int h, uint32_t levels, int min_linear, int mag_linear,
                    int clamp_u, int clamp_v, const TexMipKey *mip,
                    VkImage *img, VkDeviceMemory *mem, VkImageView *view, VkSampler *smp) {
    if (!make_image_levels((uint32_t)w, (uint32_t)h, levels, VK_FORMAT_R8G8B8A8_UNORM,
                           VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, img, mem))
        return 0;
    if (!make_view_levels(*img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, levels, view))
        return 0;
    VkSamplerCreateInfo sci;
    tex_sampler_info(min_linear, mag_linear, clamp_u, clamp_v, mip, &sci);
    VKC(vkCreateSampler(s_dev, &sci, NULL, smp));
    return 1;
}

/* The set a draw binds for cache entry `e`. Ace Mode's texture filtering (ace.h) applies to a
 * draw that minifies linearly and is transformed: through-mode draws (HUDs, menus, text) and
 * nearest-filtered textures keep the guest's sampling exactly, and render targets and the
 * snapshot never come here. The anisotropic pair is made the first time it is needed; if it
 * cannot be, the draw keeps the plain set. */
static VkDescriptorSet tex_entry_set(TexEnt *e) {
    if (!s_ace_af) return e->set;
    if (!(tex_filter_variant(s_ge->tex_filter) & 2u) || (s_ge->vtype & (1u << 23))) return e->set;
    if (!e->set_af) {
        VkSamplerCreateInfo sci;
        tex_sampler_info(1, (tex_filter_variant(s_ge->tex_filter) & 1u) != 0,
                         s_ge->tex_wrap & 1, (s_ge->tex_wrap >> 8) & 1, &e->mip, &sci);
        sci.anisotropyEnable = VK_TRUE;
        sci.maxAnisotropy = (float)s_ace_af;
        if (vkCreateSampler(s_dev, &sci, NULL, &e->smp_af) != VK_SUCCESS) {
            e->smp_af = VK_NULL_HANDLE;
            ace_note_af_failure();
            return e->set;
        }
        e->set_af = make_descriptor(e->view, e->smp_af, s_dpool_tex);
        if (!e->set_af) {
            vkDestroySampler(s_dev, e->smp_af, NULL);
            e->smp_af = VK_NULL_HANDLE;
            ace_note_af_failure();
            return e->set;
        }
        s_cnt_af_samplers++;
    }
    return e->set_af;
}

/* The current texture's mip state; all zero unless ge_tex_mip_active(). */
static void tex_mip_key(TexMipKey *k) {
    const GeState *g = s_ge;
    memset(k, 0, sizeof(*k));
    if (!ge_tex_mip_active(g)) return;
    k->max_level = g->tex_max_level;
    k->mode = g->tex_level_mode;
    k->linear = (g->tex_filter >> 1) & 1u;
    /* The per-fragment modes take the bias from the push constants: one chain and sampler
     * serve every bias. */
    k->bias = tex_lod_per_fragment(k->mode) ? 0.0f : g->tex_lod_bias;
    for (uint32_t i = 0; i < k->max_level; i++) {
        k->addr[i] = g->tex_mip_addr[i];
        k->bufw[i] = g->tex_mip_bufw[i];
        k->wlog2[i] = g->tex_mip_wlog2[i];
        k->hlog2[i] = g->tex_mip_hlog2[i];
    }
}

static uint32_t log2_pow2(uint32_t v) {
    uint32_t l = 0;
    while (v > 1u) { v >>= 1; l++; }
    return l;
}

/* Whether the current state's mip levels fit one Vulkan mip chain and sampler (#1373). In
 * the automatic and slope modes psp.frag picks the level per fragment in the PSP's 1/16-level
 * arithmetic (ge.c tex_detail16, F_LODFRAG), so every mode fits. A chain halves every level
 * down to 1x1, while the GE takes each level's size from its own TEXSIZE: a level whose
 * size is not max(1, level-0 size >> level) on both axes, or more levels than a halving
 * chain of level 0 has, cannot be sampled from one image either.
 * gpu_state_unsupported_reason() sends such draws to the software rasterizer, which samples
 * every level as given. A state without mip levels, or one drawn untextured, always fits. */
static int tex_mip_chain_supported(void) {
    const GeState *g = s_ge;
    if (!g->tex_enable || g->clear || !g->tex_addr || !ge_tex_mip_active(g)) return 1;
    if (g->tex_w <= 0 || g->tex_h <= 0 || g->tex_w > 512 || g->tex_h > 512) return 1;
    uint32_t wl = log2_pow2((uint32_t)g->tex_w), hl = log2_pow2((uint32_t)g->tex_h);
    if (g->tex_max_level > (wl > hl ? wl : hl)) return 0;
    for (uint32_t level = 1; level <= g->tex_max_level; level++) {
        if (g->tex_mip_wlog2[level - 1u] != (wl > level ? wl - level : 0u) ||
            g->tex_mip_hlog2[level - 1u] != (hl > level ? hl - level : 0u))
            return 0;
    }
    return 1;
}

/* A mip level inside a render target the GPU drew after its last readback would decode stale
 * guest VRAM: read that target back first, as build_state does for a level 0 it cannot bind. */
static void tex_mip_targets_readback(const TexMipKey *mip, const uint32_t *level_bytes) {
    for (uint32_t i = 0; i < mip->max_level; i++) {
        if (!is_vram(mip->addr[i])) continue;
        uint32_t off = vram_off(mip->addr[i]);
        for (int t = 0; t < MAX_TGT; t++) {
            Target *tg = &s_tgts[t];
            if (!tg->used || !tg->gpu_valid) continue;
            uint32_t flen = tg->stride * FB_H * (tg->fmt == 3u ? 4u : 2u);
            if (off < tg->fba + flen && tg->fba < off + level_bytes[i]) (void)target_readback(tg);
        }
    }
}

static VkDescriptorSet tex_get(void) {
    int w = s_ge->tex_w, h = s_ge->tex_h;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return s_white_set;
    unsigned filter = tex_filter_variant(s_ge->tex_filter);
    int min_linear = (filter & 2u) != 0, mag_linear = (filter & 1u) != 0;
    int clamp_u = s_ge->tex_wrap & 1, clamp_v = (s_ge->tex_wrap >> 8) & 1;
    uint64_t key = (uint64_t)s_ge->tex_addr | ((uint64_t)s_ge->tex_fmt << 32)
                 | ((uint64_t)(uint32_t)w << 36) | ((uint64_t)(uint32_t)h << 46)
                 | ((uint64_t)s_ge->tex_swizzle << 56) | ((uint64_t)filter << 57)
                 | ((uint64_t)(unsigned)clamp_u << 59) | ((uint64_t)(unsigned)clamp_v << 60);
    key ^= (uint64_t)s_ge->tex_bufw << 16;
    if (s_ge->tex_fmt >= 4 && s_ge->tex_fmt <= 7)
        key ^= clut_hash() | 1;        /* distinct entry per (texture, palette) pair */
    /* Mip levels (#1373): every field that shapes the chain or its sampler is compared
     * exactly with the key; the CLUT is shared by every level, so the key's palette hash
     * covers the levels too (the multi-CLUT bit is not modelled). */
    TexMipKey mip;
    uint32_t mip_bytes[7] = {0};
    tex_mip_key(&mip);
    if (mip.max_level) {
        if (!tex_mip_chain_supported()) {
            /* gpu_state_unsupported_reason() routes these states to the software rasterizer. */
            fprintf(stderr, "gegpu: unsupported mip chain reached the texture cache (#1373)\n");
            return s_white_set;
        }
        for (uint32_t i = 0; i < mip.max_level; i++) mip_bytes[i] = tex_level_source_size(i + 1u);
        tex_mip_targets_readback(&mip, mip_bytes);
    }
    uint32_t levels = mip.max_level + 1u;
    uint64_t hash = tex_hash();
    if (mip.max_level) hash = tex_mip_hash(hash, &mip, mip_bytes);
    uint64_t lookup_started = cpu_profile_now();
    for (int i = 0; i < s_tex_n; i++) {
        TexEnt *e = &s_tex[i];
        if (e->key != key || memcmp(&e->mip, &mip, sizeof(mip)) != 0) continue;
        cpu_profile_add(GEGPU_CPU_OBJECT_LOOKUP, lookup_started);
        if (s_cpu_profile) s_cpu_profile_stats.texture_hits++;
        e->lru = s_texlru++;
        if (e->content_valid && e->hash == hash) {
            if (sr_perf_enabled) sr_perf_texture_cache(1);
            e->pending = 1;
            return tex_entry_set(e);
        }
        if (!e->content_valid && e->hash == hash && tex_shadow_matches(e)) {
            e->content_valid = 1;
            tex_note_valid();
            e->pending = 1;
            s_cnt_shadow_avoided++;
            if (sr_perf_enabled) sr_perf_texture_cache(1);
            return tex_entry_set(e);
        }
        /* same texture state, new contents: update in place (cache stays bounded) */
        if (e->pending) submit_pending();
        if (!e->content_valid) s_cnt_shadow_required++;
        if (!tex_decode_upload(e->img, e->w, e->h, levels)) return s_white_set;
        e->hash = hash;
        e->content_valid = 1;
        tex_note_valid();
        tex_shadow_store(e);
        e->pending = 1;
        s_cnt_texup++;
        return tex_entry_set(e);
    }

    cpu_profile_add(GEGPU_CPU_OBJECT_LOOKUP, lookup_started);
    if (s_cpu_profile) s_cpu_profile_stats.texture_misses++;

    if (s_tex_n >= MAX_TEX) {
        submit_pending();   /* batches reference sets about to be freed */
        tex_evict_lru();
    }
    TexEnt *e = &s_tex[s_tex_n];
    memset(e, 0, sizeof(*e));
    if (!tex_make(w, h, levels, min_linear, mag_linear, clamp_u, clamp_v, &mip,
                  &e->img, &e->mem, &e->view, &e->smp))
        return s_white_set;
    if (!tex_decode_upload(e->img, w, h, levels)) return s_white_set;
    e->set = make_descriptor(e->view, e->smp, s_dpool_tex);
    if (!e->set) return s_white_set;
    e->key = key; e->hash = hash;
    e->w = w; e->h = h;
    e->addr = s_ge->tex_addr;
    e->bytes = tex_source_size();
    e->mip = mip;
    memcpy(e->mip_bytes, mip_bytes, sizeof(e->mip_bytes));
    tex_ent_set_ranges(e);
    tex_clean_forget(e);
    e->content_valid = 1;
    tex_note_valid();
    tex_shadow_store(e);
    e->pending = 1;
    e->lru = s_texlru++;
    s_cnt_texup++;
    s_tex_n++;
    return tex_entry_set(e);
}

/* ---- capture --------------------------------------------------------------------------- */

static float snap16(float v) { return floorf(v * 16.0f + 0.375f) * (1.0f / 16.0f); }

static void put_vert(float x, float y, float z, float rw, float u, float v, float fog,
                     int r, int g, int b, int a) {
    GpuVert *o = &s_vcpu[s_nverts++];
    o->x = snap16(x); o->y = snap16(y); o->z = z; o->rw = rw;
    o->u = u; o->v = v; o->fog = fog;
    uint32_t cr = (uint32_t)(r < 0 ? 0 : r > 255 ? 255 : r);
    uint32_t cg = (uint32_t)(g < 0 ? 0 : g > 255 ? 255 : g);
    uint32_t cb = (uint32_t)(b < 0 ? 0 : b > 255 ? 255 : b);
    uint32_t ca = (uint32_t)(a < 0 ? 0 : a > 255 ? 255 : a);
    o->rgba = cr | (cg << 8) | (cb << 16) | (ca << 24);
}

/* Ensure s_snapimg holds the CURRENT contents of `src`: pending batches submit first,
 * then the image is copied unless the previous snapshot already matches. Serves both
 * self-sampling (feedback) textures and the shader-blend destination read. */
static int snapshot_refresh(Target *src) {
    uint64_t target_started = cpu_profile_now();
    if (!src) {
        cpu_profile_add(GEGPU_CPU_SNAPSHOT_TARGET, target_started);
        return 0;
    }
    cpu_profile_add(GEGPU_CPU_SNAPSHOT_TARGET, target_started);
    s_replay_stats.snapshot_requests++;
    if (s_cpu_profile) s_cpu_profile_stats.snapshot_requests++;
    if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SNAPSHOT_REQUEST, 1);
    if (s_nbatch) submit_pending();
    uint64_t decision_started = cpu_profile_now();
    if (s_snap_src == src && s_snap_srcgen == src->render_gen) {
        cpu_profile_add(GEGPU_CPU_SNAPSHOT_DECISION, decision_started);
        if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SNAPSHOT_CACHE_HIT, 1);
        return 1;
    }
    cpu_profile_add(GEGPU_CPU_SNAPSHOT_DECISION, decision_started);
    if (cmd_batch_begin(0)) {
        /* Slot recycling/fence retirement happens inside cmd_batch_begin(). Start the
         * CPU-only region timer afterwards so GPU waits stay in boundary telemetry. */
        uint64_t region_started = cpu_profile_now();
        snapshot_copy_record(src);
        cpu_profile_add(GEGPU_CPU_SNAPSHOT_REGION, region_started);
        if (!cmd_batch_record(SR_PERF_GE_SNAPSHOT_COPY)) return 0;
        s_replay_stats.snapshot_copies++;
        if (s_cpu_profile) s_cpu_profile_stats.snapshot_copies++;
        if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SNAPSHOT_COPIED, 1);
    }
    uint64_t metadata_started = cpu_profile_now();
    snapshot_note_copied(src);
    s_cnt_snap++;
    cpu_profile_add(GEGPU_CPU_SNAPSHOT_METADATA, metadata_started);
    return 1;
}

/* PSP blend factor -> VkBlendFactor + shader premultiply flags. Never fails: factors
 * with no VK equivalent are approximated (see file header). */
static int map_factor(uint32_t f, int src_side, uint32_t fixed, int *need_const, int *premul) {
    switch (f & 0xF) {
        case 0: return src_side ? VK_BLEND_FACTOR_DST_COLOR : VK_BLEND_FACTOR_SRC_COLOR;
        case 1: return src_side ? VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 2: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 3: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 4: return VK_BLEND_FACTOR_DST_ALPHA;
        case 5: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        /* 6-9: blend_needs_shader routes every doubled alpha factor to the shader blend;
         * these mappings remain only as the fallback approximation. */
        case 6:   /* 2*src alpha: the shader premultiplies rgb on the src side */
            if (src_side) { *premul = F_SA2X; return VK_BLEND_FACTOR_ONE; }
            return VK_BLEND_FACTOR_SRC_ALPHA;                  /* dst side: halved approx */
        case 7:
            if (src_side) { *premul = F_SA2XI; return VK_BLEND_FACTOR_ONE; }
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 8: return VK_BLEND_FACTOR_DST_ALPHA;              /* 2*dst alpha approx */
        case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        default:
            if ((fixed & 0xFFFFFF) == 0)        return VK_BLEND_FACTOR_ZERO;
            if ((fixed & 0xFFFFFF) == 0xFFFFFF) return VK_BLEND_FACTOR_ONE;
            *need_const = 1;
            return VK_BLEND_FACTOR_CONSTANT_COLOR;
    }
}

/* Two DISTINCT FIX constants: VK has a single blend-constant register. */
static int blend_dual_fix(uint32_t sfp, uint32_t dfp, uint32_t fixa, uint32_t fixb) {
    return sfp >= 10 && dfp >= 10 && ((fixa ^ fixb) & 0xFFFFFFu) != 0;
}

/* The blend states fixed-function Vulkan cannot express exactly for ANY framebuffer
 * (build_state adds the per-frame conditions: a 16-bit framebuffer and dither+blend, where
 * the PSP quantizes/dithers the blend RESULT on store, so the shader must see the final
 * colour). Shared with the blend parity matrix so both classify a state the same way.
 *  - absdiff (eq 5): |src - dst| needs both subtraction orders
 *  - equations 6 and 7, which pass the source colour through (PSP-GE-RASTER-001)
 *  - every doubled alpha factor (6-9): on the PSP 2a runs to 510 and 255 - 2a goes
 *    negative (PSP-GE-RASTER-001), which no VK factor or clamped premultiply reproduces
 *  - two DISTINCT FIX constants
 * min/max (eq 3/4) are exact in fixed function: both PSP and VK ignore the factors. */
static int blend_needs_shader(uint32_t sfp, uint32_t dfp, uint32_t eq, uint32_t fixa, uint32_t fixb) {
    return eq >= 5 || (sfp >= 6 && sfp <= 9) || (dfp >= 6 && dfp <= 9) ||
           blend_dual_fix(sfp, dfp, fixa, fixb);
}

/* Build pipeline key + push constants for the current GE state. Always succeeds. */
static void build_state(int persp, int sprite, Batch *b) {
    uint64_t key_started = cpu_profile_now();
    if (s_cpu_profile) s_cpu_profile_stats.state_key_builds++;
    GeState *g = s_ge;
    PipeKey *k = &b->key;
    memset(b, 0, sizeof(*b));

    int clear = g->clear;
    int premul = 0;

    /* scissor: the GE state drives the rect; only clamp to the target image bounds
     * (512-wide render targets legitimately draw in columns 480..511 — the 480-pixel
     * crop is a PRESENTATION property, not a rasterization limit) */
    int sx1 = g->scis_x1 > 0 ? g->scis_x1 : 0, sy1 = g->scis_y1 > 0 ? g->scis_y1 : 0;
    int sx2 = g->scis_x2 < FB_W - 1 ? g->scis_x2 : FB_W - 1;
    int sy2 = g->scis_y2 < FB_H - 1 ? g->scis_y2 : FB_H - 1;
    if (sx1 > sx2 || sy1 > sy2) { b->sw = 0; return; }   /* nothing drawable */
    b->sx = sx1; b->sy = sy1; b->sw = sx2 - sx1 + 1; b->sh = sy2 - sy1 + 1;

    /* color write mask (partial-byte masks approximate: >= 0x80 disables the channel) */
    if (clear) {
        k->cmask = ((g->clear_mode & 0x100) ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                               VK_COLOR_COMPONENT_B_BIT) : 0)
                 | ((g->clear_mode & 0x200) ? VK_COLOR_COMPONENT_A_BIT : 0);
    } else {
        uint32_t mr = g->mask_rgb & 0xFF, mg = (g->mask_rgb >> 8) & 0xFF, mb = (g->mask_rgb >> 16) & 0xFF;
        uint32_t ma = (uint32_t)g->mask_alpha & 0xFF;
        (void)ma;
        /* Outside clear mode PSP alpha is the stencil plane: a draw without the stencil
         * test preserves destination alpha, and one with it stores the shader's stencil
         * value, which already carries the alpha write mask. */
        k->cmask = (mr >= 0x80 ? 0 : VK_COLOR_COMPONENT_R_BIT) | (mg >= 0x80 ? 0 : VK_COLOR_COMPONENT_G_BIT)
                 | (mb >= 0x80 ? 0 : VK_COLOR_COMPONENT_B_BIT);
        if (g->stencil_enable) k->cmask |= VK_COLOR_COMPONENT_A_BIT;
    }
    const int stencil = !clear && g->stencil_enable;

    /* blending: fixed-function VK when it maps exactly, shader blend otherwise */
    int shblend = 0;
    b->bconst[3] = 1.0f;
    if (!clear && g->blend_enable) {
        uint32_t sfp = g->blend_mode & 0xF, dfp = (g->blend_mode >> 4) & 0xF;
        uint32_t eq = (g->blend_mode >> 8) & 7;
        /* blend_needs_shader() names the states fixed-function Vulkan cannot express for
         * any framebuffer; a 16-bit framebuffer and dither+blend route to the shader too
         * (the PSP quantizes/dithers the blend RESULT on store) */
        int dual_fix = blend_dual_fix(sfp, dfp, g->blend_fixa, g->blend_fixb);
        /* A stencil-fail pixel keeps its colour, and the stored alpha is the stencil, not a
         * blend result: with the stencil test on, the shader blends too. */
        shblend = stencil || (g->fbfmt & 3) != 3 || (g->dither_enable != 0) ||
                  blend_needs_shader(sfp, dfp, eq, g->blend_fixa, g->blend_fixb);
        if (shblend) {
            if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SHBLEND_STATE, 1);
            if ((g->fbfmt & 3) != 3) {
                if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SHBLEND_FB16, 1);
            }
            if (g->dither_enable != 0) {
                if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SHBLEND_DITHER, 1);
            }
            if (eq == 5) {
                if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SHBLEND_ABSDIFF, 1);
            }
            if (sfp == 8 || sfp == 9) {
                if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SHBLEND_DOUBLE_DST_ALPHA, 1);
            }
            if (dfp >= 6 && dfp <= 9) {
                if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SHBLEND_DOUBLE_SRC_ALPHA_DST, 1);
            }
            if (dual_fix) {
                if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SHBLEND_DUAL_FIX, 1);
            }
            /* pipeline blending stays OFF; psp.frag evaluates ge.c blend_chan() against
             * the destination snapshot (refreshed below, after texture binding) */
            b->pc.bl[0] = (int32_t)(sfp | (dfp << 8) | (eq << 16));
            b->pc.fixa[0] = (float)(g->blend_fixa & 0xFF) / 255.0f;
            b->pc.fixa[1] = (float)((g->blend_fixa >> 8) & 0xFF) / 255.0f;
            b->pc.fixa[2] = (float)((g->blend_fixa >> 16) & 0xFF) / 255.0f;
            b->pc.fixb[0] = (float)(g->blend_fixb & 0xFF) / 255.0f;
            b->pc.fixb[1] = (float)((g->blend_fixb >> 8) & 0xFF) / 255.0f;
            b->pc.fixb[2] = (float)((g->blend_fixb >> 16) & 0xFF) / 255.0f;
        } else {
            int nc_a = 0, nc_b = 0;
            int sf = map_factor(sfp, 1, g->blend_fixa, &nc_a, &premul);
            int df = map_factor(dfp, 0, g->blend_fixb, &nc_b, &premul);
            /* min/max ignore the blend factors (fixed function and ge.c alike), so the
             * source premultiply, which exists only to emulate a factor, must not scale
             * the colour that min/max compare */
            if (eq == 3 || eq == 4) premul = 0;
            uint32_t fixed = nc_a ? g->blend_fixa : g->blend_fixb;
            b->bconst[0] = (float)(fixed & 0xFF) / 255.0f;
            b->bconst[1] = (float)((fixed >> 8) & 0xFF) / 255.0f;
            b->bconst[2] = (float)((fixed >> 16) & 0xFF) / 255.0f;
            k->blend_on = 1;
            k->srcf = (uint8_t)sf; k->dstf = (uint8_t)df;
            switch (eq) {
                case 1:  k->eq = VK_BLEND_OP_SUBTRACT; break;
                case 2:  k->eq = VK_BLEND_OP_REVERSE_SUBTRACT; break;
                case 3:  k->eq = VK_BLEND_OP_MIN; break;      /* exact: factors ignored */
                case 4:  k->eq = VK_BLEND_OP_MAX; break;      /* on the PSP and in VK  */
                default: k->eq = VK_BLEND_OP_ADD; break;
            }
        }
    }

    /* depth */
    static const uint8_t zmap[8] = {
        VK_COMPARE_OP_NEVER, VK_COMPARE_OP_ALWAYS, VK_COMPARE_OP_EQUAL, VK_COMPARE_OP_NOT_EQUAL,
        VK_COMPARE_OP_LESS, VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_GREATER_OR_EQUAL,
    };
    if (clear) {
        /* clear mode (sprites AND triangles): bit 10 = unconditional depth fill, no test */
        k->ztest = k->zwrite = (g->clear_mode & 0x400) ? 1 : 0;
        k->zfunc = VK_COMPARE_OP_ALWAYS;
    } else {
        k->ztest = g->ztest_enable ? 1 : 0;
        k->zwrite = (k->ztest && !g->zwrite_disable) ? 1 : 0;
        k->zfunc = k->ztest ? zmap[g->ztest & 7] : VK_COMPARE_OP_ALWAYS;
    }

    /* culling: draw_prim already reordered vertices; rasterizer keeps positive winding */
    k->cull = (!sprite && g->cull_enable && !clear) ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;

    /* fragment config */
    int textured = g->tex_enable && !clear && g->tex_addr;
    unsigned filter_variant = tex_filter_variant(g->tex_filter);
    int flags = premul;
    uint32_t lodcfg = 0u;                  /* cfg.y bits 24-31 under F_LODFRAG */
    if (clear) flags |= F_CLEAR;
    if (persp && !(clear && sprite)) flags |= F_PERSP;   /* minz/maxz discard */
    if (persp && g->fog_enable && !clear) flags |= F_FOG;
    b->pc.texsize[0] = b->pc.texsize[1] = 1.0f;
    if (textured) {
        flags |= F_TEX;
        if (g->tex_func & 0x100)   flags |= F_RGBA;
        if (g->tex_func & 0x10000) flags |= F_DBL;
        if (filter_variant == 0u) flags |= F_NEAREST;
        b->pc.texsize[0] = (float)g->tex_w;
        b->pc.texsize[1] = (float)g->tex_h;
        /* Automatic or slope level of detail, per fragment in psp.frag (#1373). Such a state
         * always samples a cached chain: a render target bound directly has no levels. */
        if (ge_tex_mip_active(g) && tex_lod_per_fragment(g->tex_level_mode)) {
            flags |= F_LODFRAG;
            lodcfg = (g->tex_level_mode == 2u ? 1u : 0u) | (((g->tex_filter >> 1) & 1u) << 1) |
                     ((g->tex_max_level & 7u) << 2);
            b->pc.texenv[3] = g->tex_lod_slope;
            b->pc.fogcol[3] = (float)(int)(g->tex_lod_bias * 16.0f);
        }
    }
    /* Native 16-bit precision emulation: dither offset + lattice snap in the shader.
     * The dither matrix rides in the push constants (one packed row per int). */
    if (g->dither_enable) {
        flags |= F_DITHER;
        for (int row = 0; row < 4; row++)
            b->pc.dith[row] = (int32_t)((uint32_t)(uint8_t)g->dith[row][0]
                            | ((uint32_t)(uint8_t)g->dith[row][1] << 8)
                            | ((uint32_t)(uint8_t)g->dith[row][2] << 16)
                            | ((uint32_t)(uint8_t)g->dith[row][3] << 24));
    }
    flags |= (int)(g->fbfmt & 3) << F_FMT_SHIFT;
    if (shblend) flags |= F_SHBLEND;
    b->pc.bl[1] = (int32_t)s_scale;   /* guest-pixel dither keying at higher scales */
    if (stencil) {
        flags |= F_STENCIL;
        b->pc.bl[2] = (int32_t)(g->stencil_test & 0xFFFFFFu);
        b->pc.bl[3] = (int32_t)((g->stencil_op & 0x070707u) | ((uint32_t)(g->mask_alpha & 0xFF) << 24));
    }

    b->pc.cfg[1] = (int32_t)(((!clear && g->atest_enable) ? (g->atest & 0xFFFFFFu) : 1u /* ALWAYS */) |
                             (lodcfg << 24));
    b->pc.cfg[2] = (int32_t)g->minz;
    b->pc.cfg[3] = (int32_t)g->maxz;
    b->pc.texenv[0] = (float)(g->tex_env & 0xFF) / 255.0f;
    b->pc.texenv[1] = (float)((g->tex_env >> 8) & 0xFF) / 255.0f;
    b->pc.texenv[2] = (float)((g->tex_env >> 16) & 0xFF) / 255.0f;
    b->pc.fogcol[0] = (float)(g->fog_color & 0xFF) / 255.0f;
    b->pc.fogcol[1] = (float)((g->fog_color >> 8) & 0xFF) / 255.0f;
    b->pc.fogcol[2] = (float)((g->fog_color >> 16) & 0xFF) / 255.0f;
    cpu_profile_add(GEGPU_CPU_STATE_KEY, key_started);

    /* texture binding: a render target sampled directly (render-to-texture), or the
     * RAM-texture cache, or white for untextured */
    b->dset = s_white_set;
    if (textured) {
        uint64_t lookup_started = cpu_profile_now();
        Target *src = NULL;
        if (is_vram(g->tex_addr)) {
            uint32_t ta = vram_off(g->tex_addr);
            for (int i = 0; i < MAX_TGT; i++) {
                Target *ti = &s_tgts[i];
                if (!ti->used || !ti->gpu_valid) continue;
                uint32_t bpp_t = ti->fmt == 3 ? 4 : 2;
                uint32_t flen = ti->stride * FB_H * bpp_t;
                if (ta >= ti->fba && ta < ti->fba + flen) { src = ti; break; }
            }
        }
        cpu_profile_add(GEGPU_CPU_OBJECT_LOOKUP, lookup_started);
        if (src) {
            static int nortt = -1;
            if (nortt < 0) { const char *nv = getenv("SR_GPU_NORTT");
                             nortt = (nv && nv[0] && strcmp(nv, "0") != 0) ? 1 : 0; }
            uint32_t bpp_s = src->fmt == 3 ? 4 : 2;
            uint32_t toff = vram_off(g->tex_addr) - src->fba;
            s_cnt_rtt++;
            /* GPU-direct: bind the target image, addressing any pixel-aligned sub-rect
             * through the texel offset in texsize.zw — no readback, no CPU decode.
             * tex_fmt must EQUAL the target's psm: reinterpreting one 16-bit format as
             * another scrambles channels/alpha and must go through VRAM pack/unpack.
             * SR_GPU_NORTT=1 forces the readback path for all draws (debug bisect). A target
             * image has no mip levels, so a draw with mip levels on (#1373) reads the target
             * back and samples its chain from the texture cache. */
            (void)target_sync_cpu_writes(src);   /* a failure leaves it invalid: decoded from VRAM */
            if (src->gpu_valid && !nortt && !ge_tex_mip_active(g) && g->tex_fmt == src->fmt && !g->tex_swizzle &&
                (g->tex_bufw == src->stride || g->tex_bufw == 0) && (toff % bpp_s) == 0) {
                uint32_t pix = toff / bpp_s;
                if (src == s_cur) {
                    /* feedback loop: sample a snapshot copy; reuse the previous snapshot
                     * while nothing new has rendered into the target */
                    snapshot_refresh(src);
                    b->dset = s_snap_set[filter_variant];
                } else {
                    if (s_nbatch && src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                        submit_pending();
                    if (src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && cmd_begin()) {
                        to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION);
                    }
                    b->dset = src->set[filter_variant];
                }
                /* texel coords map 1:1 onto the 512x272 image, shifted by the sub-rect */
                b->pc.texsize[0] = (float)FB_W;
                b->pc.texsize[1] = (float)FB_H;
                b->pc.texsize[2] = (float)(pix % src->stride);
                b->pc.texsize[3] = (float)(pix / src->stride);
            } else {
                /* incompatible stride/format reinterpretation, or mip levels: VRAM + decoder */
                target_readback(src);
                b->dset = tex_get();
            }
        } else {
            b->dset = tex_get();
        }
    }
    /* Shader blend reads the destination through the snapshot. submit_pending copies it right
     * before this state's batch when the batch reads anything drawn since the last copy, so
     * the batch reads what a copy taken here would have held. Draws batched under this one
     * state blend against that snapshot (intra-batch overlap reads the pre-batch destination,
     * the documented shader-blend hazard). */
    if (shblend || stencil) {
        s_replay_stats.snapshot_requests++;
        if (s_cpu_profile) s_cpu_profile_stats.snapshot_requests++;
        if (sr_perf_enabled) sr_perf_ge_event(SR_PERF_GE_SNAPSHOT_REQUEST, 1);
    }
    b->pc.cfg[0] = (int32_t)((g->tex_func & 7) | ((uint32_t)flags << 8));
}

/* ---- built-state template (state rebuilt per CHANGE, not per primitive) ------------------ */

typedef struct {
    uint32_t clear, clear_mode;
    int blend_enable; uint32_t blend_mode, fixa, fixb;
    uint32_t mask_rgb; int mask_alpha;
    int scis[4];
    int ztest_enable, zwrite_disable; uint32_t ztest, minz, maxz;
    int cull_enable; uint32_t cull;
    int tex_enable; uint32_t tex_addr, tex_bufw, tex_fmt, tex_wrap, tex_func, tex_env, tex_filter;
    int tex_w, tex_h; int tex_swizzle;
    /* Mip levels (#1373): every field tex_get keys a chain or its sampler by. */
    uint32_t tex_max_level, tex_level_mode; float tex_lod_bias, tex_lod_slope;
    uint32_t tex_mip_addr[7], tex_mip_bufw[7]; uint8_t tex_mip_wlog2[7], tex_mip_hlog2[7];
    uint32_t clut_fmt, clut_gen;
    int atest_enable; uint32_t atest;
    int stencil_enable; uint32_t stencil_test, stencil_op;
    int fog_enable; uint32_t fog_color;
    uint32_t fbfmt;
    int dither_enable; int8_t dith[4][4];
    int persp, sprite;
    uint32_t flushgen;
} StateSnap;
static StateSnap s_snapst;
static Batch     s_tmpl;
static int       s_tmpl_ok = 0;

static void snap_fill(StateSnap *o, int persp, int sprite) {
    GeState *g = s_ge;
    memset(o, 0, sizeof(*o));
    o->clear = (uint32_t)g->clear; o->clear_mode = g->clear_mode;
    o->blend_enable = g->blend_enable; o->blend_mode = g->blend_mode;
    o->fixa = g->blend_fixa; o->fixb = g->blend_fixb;
    o->mask_rgb = g->mask_rgb; o->mask_alpha = g->mask_alpha;
    o->scis[0] = g->scis_x1; o->scis[1] = g->scis_y1; o->scis[2] = g->scis_x2; o->scis[3] = g->scis_y2;
    o->ztest_enable = g->ztest_enable; o->zwrite_disable = g->zwrite_disable;
    o->ztest = g->ztest; o->minz = g->minz; o->maxz = g->maxz;
    o->cull_enable = g->cull_enable; o->cull = g->cull;
    o->tex_enable = g->tex_enable; o->tex_addr = g->tex_addr; o->tex_bufw = g->tex_bufw;
    o->tex_fmt = g->tex_fmt; o->tex_wrap = g->tex_wrap; o->tex_func = g->tex_func;
    o->tex_env = g->tex_env; o->tex_filter = g->tex_filter;
    o->tex_w = g->tex_w; o->tex_h = g->tex_h; o->tex_swizzle = g->tex_swizzle;
    o->tex_max_level = g->tex_max_level; o->tex_level_mode = g->tex_level_mode;
    o->tex_lod_bias = g->tex_lod_bias; o->tex_lod_slope = g->tex_lod_slope;
    memcpy(o->tex_mip_addr, g->tex_mip_addr, sizeof(o->tex_mip_addr));
    memcpy(o->tex_mip_bufw, g->tex_mip_bufw, sizeof(o->tex_mip_bufw));
    memcpy(o->tex_mip_wlog2, g->tex_mip_wlog2, sizeof(o->tex_mip_wlog2));
    memcpy(o->tex_mip_hlog2, g->tex_mip_hlog2, sizeof(o->tex_mip_hlog2));
    o->clut_fmt = g->clut_fmt; o->clut_gen = g->clut_gen;
    o->atest_enable = g->atest_enable; o->atest = g->atest;
    o->stencil_enable = g->stencil_enable;
    o->stencil_test = g->stencil_test; o->stencil_op = g->stencil_op;
    o->fog_enable = g->fog_enable; o->fog_color = g->fog_color;
    o->fbfmt = g->fbfmt;
    o->dither_enable = g->dither_enable;
    memcpy(o->dith, g->dith, sizeof(o->dith));
    o->persp = persp; o->sprite = sprite;
    o->flushgen = s_flushgen;
}

/* ge_state_serial() when s_snapst was last filled or matched.  snap_fill reads only GE state,
 * persp, sprite and s_flushgen, so while none of them moved it would reproduce s_snapst exactly
 * and the template still applies: the primitives of one draw skip the fill and compare. */
static uint32_t s_snapst_serial;

static void state_get(int persp, int sprite, Batch *out) {
    uint64_t prep_started = cpu_profile_now();
    uint32_t serial = ge_state_serial();
    if (s_tmpl_ok && serial == s_snapst_serial && s_snapst.persp == persp &&
        s_snapst.sprite == sprite && s_snapst.flushgen == s_flushgen) {
        *out = s_tmpl;
        if (s_cpu_profile) s_cpu_profile_stats.state_cache_hits++;
        cpu_profile_add(GEGPU_CPU_STATE_PREP, prep_started);
        return;
    }
    StateSnap sn; snap_fill(&sn, persp, sprite);
    if (s_tmpl_ok && !memcmp(&sn, &s_snapst, sizeof(sn))) {
        s_snapst_serial = serial;
        *out = s_tmpl;
        if (s_cpu_profile) s_cpu_profile_stats.state_cache_hits++;
        cpu_profile_add(GEGPU_CPU_STATE_PREP, prep_started);
        return;
    }
    if (s_cpu_profile) {
        s_cpu_profile_stats.state_cache_misses++;
        cpu_profile_add(GEGPU_CPU_STATE_PREP, prep_started);
    }
    begin_target();                       /* build_state may bind sibling targets */
    build_state(persp, sprite, &s_tmpl);
    prep_started = cpu_profile_now();
    snap_fill(&s_snapst, persp, sprite);  /* re-fill: build_state may have submitted */
    s_snapst_serial = serial;             /* build_state runs no GE command */
    s_tmpl_ok = 1;
    *out = s_tmpl;
    if (s_cpu_profile) {
        /* Count one state-preparation operation per state_get(), not one per timed
         * segment on the cache-miss path. */
        cpu_profile_add_elapsed(GEGPU_CPU_STATE_PREP, SDL_GetTicksNS() - prep_started);
    }
}

/* Does a stencil batch write a value that depends on the destination, so that a primitive
 * drawn over an earlier one in the same batch would read what was there before both? It
 * does when it blends (the shader blends every stencil state), or when the stencil it
 * stores depends on the old one through the test or the op. Only the ops that can run
 * count: the stencil-fail op unless the test always passes, the depth-fail op only under
 * the depth test, and the depth-pass op unless the test never passes. Non-stencil shader
 * blending keeps its documented intra-batch hazard (header). */
static int stencil_reads_own_writes(const Batch *b) {
    const uint32_t flags = (uint32_t)b->pc.cfg[0] >> 8;
    if (!(flags & F_STENCIL)) return 0;
    if (flags & F_SHBLEND) return 1;
    const uint32_t func = (uint32_t)b->pc.bl[2] & 7u, ops = (uint32_t)b->pc.bl[3];
    const uint32_t op[3] = { ops & 7u, (ops >> 8) & 7u, (ops >> 16) & 7u };
    const int runs[3] = { func != 1u, b->key.ztest != 0, func != 0u };
    const int test_reads = func != 0u && func != 1u;
    for (int i = 0; i < 3; i++) {
        if (!runs[i] || op[i] == 0u) continue;
        if (test_reads || op[i] >= 3u) return 1;   /* invert, incr, decr read the old value */
    }
    return 0;
}

/* Box of the primitives in the last batch, kept while that batch's stencil reads its own
 * writes (append). */
static SnapRect s_tail_prims;

static void append(Batch *b, uint32_t first, uint32_t count) {
    if (s_cpu_profile) s_cpu_profile_stats.append_calls++;
    if (b->sw <= 0 || count == 0) return;
    sr_vram_mark_renderer_pending();
    if (g_ace_on && s_cur) ace_note_target_draw(s_cur->fba);
    if ((((uint32_t)b->pc.cfg[0] >> 8) & F_SHBLEND) && sr_perf_enabled)
        sr_perf_ge_event(SR_PERF_GE_SHBLEND_DRAW, 1);
    const int self_dep = stencil_reads_own_writes(b);
    SnapRect prim = !self_dep ? k_snap_rect_empty
                  : b->key.tnl ? tnl_rect(b, first, count) : verts_rect(b, first, count);
    if (s_nbatch) {
        Batch *last = &s_batch[s_nbatch - 1];
        if (s_cpu_profile) s_cpu_profile_stats.append_compare_calls++;
        if (last->first + last->count == first &&
            !memcmp(&last->key, &b->key, sizeof(b->key)) &&
            last->sx == b->sx && last->sy == b->sy && last->sw == b->sw && last->sh == b->sh &&
            !memcmp(last->bconst, b->bconst, sizeof(b->bconst)) &&
            !memcmp(&last->pc, &b->pc, sizeof(b->pc)) && last->dset == b->dset) {
            if (!self_dep || !snap_rect_overlap(&s_tail_prims, &prim)) {
                last->count += count;
                if (self_dep) snap_rect_union(&s_tail_prims, &prim);
                if (s_cpu_profile) s_cpu_profile_stats.append_merges++;
                return;
            }
            s_replay_stats.stencil_splits++;
        }
    }
    b->first = first; b->count = count;
    s_batch[s_nbatch++] = *b;
    s_tail_prims = prim;
}

static void ensure_room(uint32_t verts) {
    if (s_cpu_profile) s_cpu_profile_stats.ensure_room_calls++;
    if (s_nverts + verts > MAX_VERTS || s_nbatch >= MAX_BATCH - 1) {
        if (s_cpu_profile) s_cpu_profile_stats.ensure_room_flushes++;
        submit_pending();
    }
}

static int native_rgb_mask_exact(void) {
    uint32_t mr=s_ge->mask_rgb&0xFF, mg=(s_ge->mask_rgb>>8)&0xFF, mb=(s_ge->mask_rgb>>16)&0xFF;
    uint32_t rm,gm,bm,rfull,gfull,bfull;
    switch (s_ge->fbfmt&3) {
        case 0: rm=mr>>3; gm=mg>>2; bm=mb>>3; rfull=bfull=31; gfull=63; break;
        case 1: rm=mr>>3; gm=mg>>3; bm=mb>>3; rfull=gfull=bfull=31; break;
        case 2: rm=mr>>4; gm=mg>>4; bm=mb>>4; rfull=gfull=bfull=15; break;
        default:rm=mr; gm=mg; bm=mb; rfull=gfull=bfull=255; break;
    }
    return (rm==0||rm==rfull)&&(gm==0||gm==gfull)&&(bm==0||bm==bfull);
}

/* Vulkan now carries EVERY blend state: exact fixed function where it maps, shader
 * blending against a destination snapshot everywhere else (including post-blend 16-bit
 * quantization and post-blend dither, which run in the shader in hardware order — see
 * build_state). Only states the batch model cannot express at all remain on ge.c, the
 * behavioral arbiter: per-bit write masks, stencil, color test, and logic ops (all of
 * which read-modify-write destination bits per pixel). */
/* The shader runs the stencil test before the pipeline depth test, so a stencil-fail
 * pixel the depth test would also reject, or a depth-fail pixel, can only be dropped: the
 * ops for them must be KEEP while the depth test is on. A test that always passes never
 * runs the stencil-fail op. Clear mode ignores the stencil test (ge.c put_px_rgba). */
static int stencil_gpu_supported(void) {
    if (!s_ge->stencil_enable || s_ge->clear || !s_ge->ztest_enable) return 1;
    const uint32_t func = s_ge->stencil_test & 7u, sfail = s_ge->stencil_op & 7u;
    const uint32_t zfail = (s_ge->stencil_op >> 8) & 7u;
    return (sfail == 0u || func == 1u) && zfail == 0u;
}

static int gpu_state_unsupported_reason(void) {
    if (!stencil_gpu_supported()) return GEGPU_FALLBACK_STENCIL;
    if (s_ge->color_test_enable) return GEGPU_FALLBACK_COLOR_TEST;
    if (s_ge->logic_op_enable) return GEGPU_FALLBACK_LOGIC_OP;
    if (!s_ge->clear&&!native_rgb_mask_exact()) return GEGPU_FALLBACK_WRITE_MASK;
    /* Framebuffer placements the persistent-target model cannot represent: color
     * buffers in main RAM, and strides wider than the target images. */
    if (!is_vram(s_ge->fbp)) return GEGPU_FALLBACK_RAM_FB;
    if ((s_ge->fbw ? s_ge->fbw : 512u) > FB_W) return GEGPU_FALLBACK_WIDE_STRIDE;
    /* Mip levels one Vulkan mip chain cannot hold (#1373). */
    if (!tex_mip_chain_supported()) return GEGPU_FALLBACK_MIPS;
    return -1;
}

/* `forced` is the reason a primitive kind always falls back (lines, points), or -1 for one
 * that falls back only under an unsupported state. */
static int software_fallback_begin(int forced) {
    const int reason = forced >= 0 ? forced : gpu_state_unsupported_reason();
    if (reason < 0) return 0;
    s_replay_stats.fallback_prims++;
    if (s_nbatch&&!submit_pending()) return 1;

    Target *t=is_vram(s_ge->fbp)?target_find_by_fba(vram_off(s_ge->fbp)):NULL;
    if (t&&t->gpu_valid) {
        s_replay_stats.fallback_readbacks[reason]++;
        (void)target_readback(t);
        readback_discard_target(t);
        t->gpu_valid=0;
    }
    DepthEnt *d=t?t->dep:NULL;
    if (!d) {
        uint32_t zba=vram_off(s_ge->zbp);
        uint32_t zs=s_ge->zbw?s_ge->zbw:(s_ge->fbw?s_ge->fbw:512);
        for(int i=0;i<MAX_DEP;i++)
            if(s_deps[i].used&&s_deps[i].zba==zba&&s_deps[i].zstride==zs){d=&s_deps[i];break;}
    }
    if (d) (void)depth_to_cpu(d);
    if (s_cur==t) s_cur=NULL;
    s_tmpl_ok=0;
    return 1;
}

/* ---- primitive hooks ---------------------------------------------------------------------- */

/* Fail closed on a non-finite vertex, with the SAME rule the software rasterizer applies
 * (ge_vtx_finite in ge_shared.h, applied in ge.c before the capture seam). A NaN screen
 * position or clip-w becomes a NaN gl_Position here, and a NaN gl_Position has an
 * UNDEFINED fixed-function clipper verdict on Vulkan: the primitive is not reliably
 * clipped away, so the rasterizer can cover an unbounded area - the spikes/garbage a
 * non-finite skinned vertex produces. ge.c normally drops such a vertex before the seam
 * (its CVtx::nf verdict is stronger: it also sees the clip position and the lit colour);
 * this is the same verdict enforced on the Vulkan side, so the two GE paths cannot
 * disagree and a future caller that reaches the seam unchecked still fails closed.
 * The drop is counted like ge.c's GESTAT nonfinite= and the primitive is reported as
 * TAKEN (return 1), so the software rasterizer does not then draw what we just rejected. */
/* Two different 1s: hook_finite() returns 1 when every vertex is finite (accept); a
 * hook returns 1 when it TOOK the primitive, which includes rejecting a non-finite one
 * so the software rasterizer does not draw it either. */
static int hook_finite(const GeVtx *const *v, int n) {
    for (int i = 0; i < n; i++)
        if (!ge_vtx_finite(v[i])) { s_cnt_nonfinite++; return 0; }
    return 1;
}

static int hook_tri(const GeVtx *A, const GeVtx *B, const GeVtx *C, int persp) {
    if (!s_ready) return 0;
    cpu_profile_hook_enter();
    { const GeVtx *vv[3] = { A, B, C };
      if (!hook_finite(vv, 3)) return cpu_profile_hook_leave(1); }
    if (software_fallback_begin(-1)) return cpu_profile_hook_leave(0);
    static int nocull = -1;
    if (nocull < 0) nocull = getenv("SR_NOCULL") ? 1 : 0;
    Batch b;
    state_get(persp, 0, &b);
    if (nocull) b.key.cull = VK_CULL_MODE_NONE;
    if (!begin_target()) return cpu_profile_hook_leave(1); /* target alloc failed: drop draw */
    ensure_room(3);
    uint64_t vertex_started = cpu_profile_now();
    uint32_t first = s_nverts;
    /* flat shading: provoking vertex is the LAST one on the PSP; clear triangles always
     * use C's color (raster_tri clear branch) */
    int flat = !s_ge->shade_gouraud || s_ge->clear;
    const GeVtx *ca = flat ? C : A;
    const GeVtx *cb = flat ? C : B;
    put_vert(A->x, A->y, A->z, A->rw, A->u, A->v, A->fog, ca->r, ca->g, ca->b, ca->a);
    put_vert(B->x, B->y, B->z, B->rw, B->u, B->v, B->fog, cb->r, cb->g, cb->b, cb->a);
    put_vert(C->x, C->y, C->z, C->rw, C->u, C->v, C->fog, C->r, C->g, C->b, C->a);
    cpu_profile_vertex_done(vertex_started, 3);
    append(&b, first, 3);
    s_cnt_tri++;
    return cpu_profile_hook_leave(1);
}

/* GPU transform and lighting (#318). The arenas exist only once a PRIM has arrived: the
 * route is opt-in, and a default run allocates nothing for it. */
static int tnl_alloc(void) {
    s_tnl_vcpu = (GeTnlVtx *)malloc((size_t)MAX_TNL_VERTS * sizeof(GeTnlVtx));
    s_tnl_pcpu = (GeTnlParams *)malloc((size_t)MAX_TNL_PARAMS * sizeof(GeTnlParams));
    s_tnl_box = (TnlBox *)malloc((size_t)MAX_TNL_PARAMS * sizeof(TnlBox));
    if (!s_tnl_vcpu || !s_tnl_pcpu || !s_tnl_box) return 0;
    for (int i = 0; i < SUBMIT_FRAMES; i++) {
        SubmitSlot *slot = &s_submit[i];
        if (!make_buffer((VkDeviceSize)TNL_ARENA_VERTS * sizeof(GeTnlVtx),
                         VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &slot->tbuf, &slot->tbuf_mem,
                         (void **)&slot->tmap)) return 0;
        if (!make_buffer((VkDeviceSize)TNL_ARENA_PARAMS * sizeof(GeTnlParams),
                         VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &slot->pbuf, &slot->pbuf_mem,
                         (void **)&slot->pmap)) return 0;
        VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        dai.descriptorPool = s_tnl_dpool; dai.descriptorSetCount = 1; dai.pSetLayouts = &s_tnl_dlayout;
        if (vkAllocateDescriptorSets(s_dev, &dai, &slot->tset) != VK_SUCCESS) return 0;
        VkDescriptorBufferInfo bi = { slot->pbuf, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        w.dstSet = slot->tset; w.dstBinding = 0; w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w.pBufferInfo = &bi;
        vkUpdateDescriptorSets(s_dev, 1, &w, 0, NULL);
    }
    s_tnl_ready = 1;
    return 1;
}

/* ge.c proved the PRIM's triangles reach the rasterizer from psp_tnl.vert with the bits its
 * own T&L and hook_tri would give them; the fragment state and batching are hook_tri's. */
static int hook_tnl_prim(const GeTnlPrim *p) {
    if (!s_ready) return 0;
    if (!s_vs_tnl) {
        static int device_reported;
        if (!device_reported) {
            device_reported = 1;
            fprintf(stderr, "gegpu: SR_GPU_TNL: this device does not keep fp32 subnormals in shaders "
                            "(shaderDenormPreserveFloat32), which GPU T&L needs to match ge.c bit for "
                            "bit; ge.c keeps T&L (#318)\n");
        }
        return 0;
    }
    const uint32_t nv = p->ntri * 3u;
    if (!nv || nv > MAX_TNL_VERTS) return 0;
    if (!s_tnl_ready && !tnl_alloc()) {
        static int alloc_reported;
        if (!alloc_reported) {
            alloc_reported = 1;
            fprintf(stderr, "gegpu: GPU T&L arenas could not be allocated; ge.c keeps T&L\n");
        }
        return 0;
    }
    cpu_profile_hook_enter();
    if (software_fallback_begin(-1)) return cpu_profile_hook_leave(0);
    static int nocull = -1;
    if (nocull < 0) nocull = getenv("SR_NOCULL") ? 1 : 0;
    Batch b;
    state_get(1, 0, &b);
    if (nocull) b.key.cull = VK_CULL_MODE_NONE;
    b.key.tnl = 1;
    /* Without a target, decline: ge.c's own T&L then reaches hook_tri, which decides the
     * draw exactly as it does when GPU T&L is off. */
    if (!begin_target()) return cpu_profile_hook_leave(0);
    if (s_ntnl + nv > MAX_TNL_VERTS || s_ntnlp >= MAX_TNL_PARAMS || s_nbatch >= MAX_BATCH - 1)
        submit_pending();
    const uint32_t pi = s_ntnlp++;
    s_tnl_pcpu[pi] = *p->params;
    const uint32_t first = s_ntnl;
    for (uint32_t k = 0; k < nv; k++) {
        GeTnlVtx *o = &s_tnl_vcpu[s_ntnl++];
        *o = p->refs[p->tri[k]];
        o->param = pi;
    }
    s_tnl_box[pi] = (TnlBox){ first, s_ntnl, p->box[0], p->box[1], p->box[2], p->box[3] };
    append(&b, first, nv);
    s_cnt_tnl_prims++;
    s_cnt_tnl_tris += p->ntri;
    return cpu_profile_hook_leave(1);
}

static int hook_sprite(const GeVtx *p0, const GeVtx *p1, int persp) {
    if (!s_ready) return 0;
    cpu_profile_hook_enter();
    { const GeVtx *vv[2] = { p0, p1 };
      if (!hook_finite(vv, 2)) return cpu_profile_hook_leave(1); }
    if (software_fallback_begin(-1)) return cpu_profile_hook_leave(0);
    Batch b;
    state_get(persp, 1, &b);
    if (!begin_target()) return cpu_profile_hook_leave(1);
    float xa = floorf(fminf(p0->x, p1->x)), xb = floorf(fmaxf(p0->x, p1->x));
    float ya = floorf(fminf(p0->y, p1->y)), yb = floorf(fmaxf(p0->y, p1->y));
    if (xb <= xa || yb <= ya) return cpu_profile_hook_leave(1);

    float u0 = p0->u, v0 = p0->v, u1 = p1->u, v1 = p1->v;
    if (persp) {
        u0 = p0->u / (p0->rw != 0.0f ? p0->rw : 1.0f);
        v0 = p0->v / (p0->rw != 0.0f ? p0->rw : 1.0f);
        u1 = p1->u / (p1->rw != 0.0f ? p1->rw : 1.0f);
        v1 = p1->v / (p1->rw != 0.0f ? p1->rw : 1.0f);
    }
    /* The coordinate is linear across the rectangle, from the u of the vertex on the left
     * edge to the u of the one on the right (likewise v), and the PSP samples it at each
     * pixel's centre (PSP-GE-SAMPLE-001), which is where Vulkan samples a fragment at 1x.
     * Readback keeps the top-left device sample of each scaled guest pixel, at guest
     * offset 0.5 / scale; shift the endpoints so that sample sees the pixel-centre value. */
    float ul = (p0->x <= p1->x) ? u0 : u1, ur = (p0->x <= p1->x) ? u1 : u0;
    float vt = (p0->y <= p1->y) ? v0 : v1, vbm = (p0->y <= p1->y) ? v1 : v0;
    float uw = (ur - ul) / (xb - xa), vw = (vbm - vt) / (yb - ya);
    float centre_shift = 0.5f - 0.5f / (float)s_scale;
    float ua = ul + centre_shift * uw, ub = ur + centre_shift * uw;
    float va = vt + centre_shift * vw, vb = vbm + centre_shift * vw;

    float z = p1->z, fog = p1->fog;
    int r = p1->r, g = p1->g, bb_ = p1->b, a = p1->a;

    ensure_room(6);
    uint64_t vertex_started = cpu_profile_now();
    uint32_t first = s_nverts;
    put_vert(xa, ya, z, 1.0f, ua, va, fog, r, g, bb_, a);
    put_vert(xb, ya, z, 1.0f, ub, va, fog, r, g, bb_, a);
    put_vert(xa, yb, z, 1.0f, ua, vb, fog, r, g, bb_, a);
    put_vert(xb, ya, z, 1.0f, ub, va, fog, r, g, bb_, a);
    put_vert(xb, yb, z, 1.0f, ub, vb, fog, r, g, bb_, a);
    put_vert(xa, yb, z, 1.0f, ua, vb, fog, r, g, bb_, a);
    cpu_profile_vertex_done(vertex_started, 6);
    append(&b, first, 6);
    s_cnt_spr++;
    return cpu_profile_hook_leave(1);
}

/* Lines as 1-pixel-wide quads (the software DDA's pixel set, approximately). */
static int hook_line(const GeVtx *A, const GeVtx *B, int persp) {
    if (!s_ready) return 0;
    cpu_profile_hook_enter();
    { const GeVtx *vv[2] = { A, B };
      if (!hook_finite(vv, 2)) return cpu_profile_hook_leave(1); }
    if (software_fallback_begin(GEGPU_FALLBACK_LINE)) return cpu_profile_hook_leave(0);
    Batch b;
    state_get(persp, 1, &b);              /* sprite semantics: no culling */
    if (!begin_target()) return cpu_profile_hook_leave(1);
    float dx = B->x - A->x, dy = B->y - A->y;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f) { dx = 1.0f; dy = 0.0f; len = 1.0f; }
    float nx = -dy / len * 0.5f, ny = dx / len * 0.5f;   /* half-width perpendicular */
    float ex = dx / len * 0.5f, ey = dy / len * 0.5f;    /* endpoint extension */
    float ax = A->x + 0.5f, ay = A->y + 0.5f;            /* DDA truncates; sample centers */
    float bx = B->x + 0.5f, by = B->y + 0.5f;

    /* flat shading uses B (the provoking vertex) */
    int flat = !s_ge->shade_gouraud;
    const GeVtx *cA = flat ? B : A;

    ensure_room(6);
    uint64_t vertex_started = cpu_profile_now();
    uint32_t first = s_nverts;
    put_vert(ax - ex - nx, ay - ey - ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(ax - ex + nx, ay - ey + ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(bx + ex - nx, by + ey - ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    put_vert(ax - ex + nx, ay - ey + ny, A->z, A->rw, A->u, A->v, A->fog, cA->r, cA->g, cA->b, cA->a);
    put_vert(bx + ex + nx, by + ey + ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    put_vert(bx + ex - nx, by + ey - ny, B->z, B->rw, B->u, B->v, B->fog, B->r, B->g, B->b, B->a);
    cpu_profile_vertex_done(vertex_started, 6);
    append(&b, first, 6);
    s_cnt_line++;
    return cpu_profile_hook_leave(1);
}

static int hook_point(const GeVtx *A, int persp) {
    if (!s_ready) return 0;
    cpu_profile_hook_enter();
    { const GeVtx *vv[1] = { A };
      if (!hook_finite(vv, 1)) return cpu_profile_hook_leave(1); }
    if (software_fallback_begin(GEGPU_FALLBACK_POINT)) return cpu_profile_hook_leave(0);
    Batch b;
    state_get(persp, 1, &b);
    if (!begin_target()) return cpu_profile_hook_leave(1);
    float x0 = floorf(A->x), y0 = floorf(A->y);
    ensure_room(6);
    uint64_t vertex_started = cpu_profile_now();
    uint32_t first = s_nverts;
    put_vert(x0,        y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0,        y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0,        A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0 + 1.0f, y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    put_vert(x0,        y0 + 1.0f, A->z, A->rw, A->u, A->v, A->fog, A->r, A->g, A->b, A->a);
    cpu_profile_vertex_done(vertex_started, 6);
    append(&b, first, 6);
    s_cnt_line++;
    return cpu_profile_hook_leave(1);
}

/* ---- CPU writes: invalidate overlapping textures; merge bytes into overlapping targets ---- */

static void hook_vram_dirty(uint32_t addr, uint32_t bytes) {
    if (!s_ready || !bytes) return;
    if (g_ace_on && is_vram(addr)) ace_note_vram_write(vram_off(addr), bytes);
    /* CPU HLE calls may use any PSP address alias (KSEG mirrors for RAM or the
     * mirrored 0x04xxxxxx VRAM aperture). Compare cache ranges in the same
     * physical namespace before deciding whether a texture is stale. The
     * target path below still uses vram_off(), because framebuffer targets are
     * indexed by their 2 MiB-local VRAM offset. */
    uint32_t canonical_addr = tex_canonical(addr);
    uint64_t dirty0 = canonical_addr, dirty1 = dirty0 + bytes;
    uint64_t page0 = dirty0 >> TEX_CLEAN_PAGE_SHIFT, page1 = (dirty1 - 1u) >> TEX_CLEAN_PAGE_SHIFT;
    int indexed = page1 < TEX_CLEAN_PAGES;
    int scan = 1;
    if (indexed && tex_clean_pages(page0, page1)) {
        scan = 0;                          /* no entry reads these pages */
        s_cnt_tex_clean_skips++;
    } else if (indexed && tex_quiet_pages(page0, page1)) {
        scan = 0;                          /* every entry reading them is already invalid */
        s_tmpl_ok = 0;
        s_cnt_tex_quiet_skips++;
    }
    uint64_t span0 = page0 << TEX_CLEAN_PAGE_SHIFT, span1 = (page1 + 1u) << TEX_CLEAN_PAGE_SHIFT;
    int span_touched = 0, span_valid = 0;
    for (int i = 0; scan && i < s_tex_n; i++) {
        TexEnt *e = &s_tex[i];
        if (!tex_ent_overlaps(e, span0, span1)) continue;
        span_touched = 1;
        /* A mip chain (#1373) is stale when any of its levels is written: the same
         * canonical comparison against each level's own source range. */
        if (tex_ent_overlaps(e, dirty0, dirty1)) {
            if (e->content_valid) {
                s_cnt_tex_invalidations++;
                e->content_valid = 0;
            }
            /* The cached state template carries this texture's descriptor, and a draw
             * with unchanged GE registers takes the template fast path in state_get
             * without consulting tex_get. The notified write therefore invalidates the
             * template as well as the entry (coherence selftest texture-dirty-same-state). */
            s_tmpl_ok = 0;
        }
        if (e->content_valid) span_valid = 1;
    }
    if (scan && indexed) {
        if (!span_touched) tex_clean_set(page0, page1, 1);
        else if (!span_valid)
            for (uint64_t p = page0; p <= page1; p++) s_tex_quiet[p] = s_tex_valid_gen;
    }
    if (!is_vram(canonical_addr)) return;
    uint32_t a0 = vram_off(canonical_addr);
    for (int i = 0; i < MAX_DEP; i++) {
        DepthEnt *d = &s_deps[i];
        if (!d->used || !depth_span_overlaps(d, addr, bytes)) continue;
        d->cpu_dirty = 1;
        d->guest_dirty = 0;
    }
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used || !t->gpu_valid) continue;
        uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
        uint32_t flen = t->stride * FB_H * bpp_t;
        if (a0 < t->fba + flen && t->fba < a0 + bytes) {
            /* Held until the GE next uses the target (target_sync_cpu_writes). A readback
             * already in flight holds the image from before this write: it must not copy
             * over the bytes the CPU has just written. */
            readback_discard_target(t);
            uint32_t lo = a0 > t->fba ? a0 : t->fba;
            uint32_t hi = a0 + bytes < t->fba + flen ? a0 + bytes : t->fba + flen;
            target_note_cpu_write(t, lo, hi);
            s_cnt_dirty++;
        }
    }
}

static int hook_vram_read(uint32_t addr, uint32_t bytes) {
    if (!s_ready || !bytes) return 1;
    uint64_t read0 = SR_PHYS(addr);
    uint64_t read1 = read0 + bytes;
    if (s_nbatch) {
        int queued_overlap = 0;
        if (s_cur && s_cur->used && s_cur->gpu_valid) {
            uint32_t bpp = s_cur->fmt == 3u ? 4u : 2u;
            uint64_t target0 = UINT64_C(0x04000000) + s_cur->fba;
            uint64_t target1 = target0 + (uint64_t)s_cur->stride * FB_H * bpp;
            queued_overlap = read0 < target1 && target0 < read1;
        }
        if (!queued_overlap && s_cur && s_cur->dep)
            queued_overlap = depth_span_overlaps(s_cur->dep, addr, bytes);
        if (queued_overlap && !submit_pending()) return 0;
    }
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used || !t->gpu_valid || t->clean_gen == t->render_gen) continue;
        uint32_t bpp = t->fmt == 3u ? 4u : 2u;
        uint64_t target0 = UINT64_C(0x04000000) + t->fba;
        uint64_t target1 = target0 + (uint64_t)t->stride * FB_H * bpp;
        if (read0 < target1 && target0 < read1 && !target_readback(t)) return 0;
    }
    for (int i = 0; i < MAX_DEP; i++) {
        DepthEnt *d = &s_deps[i];
        if (d->used && d->guest_dirty && depth_span_overlaps(d, addr, bytes) &&
            !depth_readback_to_guest(d)) return 0;
    }
    return 1;
}

static int hook_vram_write_prepare(uint32_t addr, uint32_t bytes) {
    if (!s_ready || !is_vram(addr)) return 1;
    if (s_nbatch && !submit_pending()) return 0;
    for (int i = 0; i < MAX_DEP; i++) {
        DepthEnt *d = &s_deps[i];
        if (d->used && d->guest_dirty && depth_span_overlaps(d, addr, bytes) &&
            !depth_readback_to_guest(d)) return 0;
    }
    return 1;
}

static int hook_vram_pending(void) {
    if (s_nbatch) return 1;
    for (int i = 0; i < MAX_TGT; i++) {
        const Target *t = &s_tgts[i];
        if (t->used && t->gpu_valid && t->clean_gen != t->render_gen) return 1;
    }
    for (int i = 0; i < MAX_DEP; i++)
        if (s_deps[i].used && s_deps[i].guest_dirty) return 1;
    return 0;
}

/* ---- GE block transfer: GPU-side image blit ------------------------------------------------ */

/* Find the target whose address range contains VRAM offset `a` (gpu_valid not required). */
static Target *target_containing(uint32_t a) {
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used) continue;
        uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
        if (a >= t->fba && a < t->fba + t->stride * FB_H * bpp_t) return t;
    }
    return NULL;
}

/* Perform the block transfer as a vkCmdCopyImage between two framebuffer targets. Returns 1
 * on success (guest VRAM left stale for the destination, like any rendered-to target); 0
 * falls back to the CPU copy in ge.c (readback + memmove + dirty-invalidate). */
static int hook_xfer(uint32_t startdata) {
    if (!s_ready) return 0;
    uint32_t srcBase = (s_ge->xf_src & 0xFFFFF0u) | ((s_ge->xf_srcw & 0xFF0000u) << 8);
    uint32_t dstBase = (s_ge->xf_dst & 0xFFFFF0u) | ((s_ge->xf_dstw & 0xFF0000u) << 8);
    if (!is_vram(srcBase) || !is_vram(dstBase))
        return 0;                                       /* RAM endpoint: CPU path */
    uint32_t srcStride = s_ge->xf_srcw & 0x7F8u;
    uint32_t dstStride = s_ge->xf_dstw & 0x7F8u;
    uint32_t bpp = (startdata & 1) ? 4 : 2;
    Target *src = target_containing(vram_off(srcBase));
    Target *dst = target_containing(vram_off(dstBase));
    if (!src || !dst || src == dst || !src->gpu_valid) return 0;
    uint32_t bpp_s = src->fmt == 3 ? 4 : 2;
    /* raw-byte copy must match the converted RGBA images texel-for-texel */
    if (src->fmt != dst->fmt || bpp_s != bpp) return 0;
    if (src->stride != srcStride || dst->stride != dstStride) return 0;
    uint32_t soff = (vram_off(srcBase) - src->fba) / bpp;
    uint32_t doff = (vram_off(dstBase) - dst->fba) / bpp;
    uint32_t sx = (s_ge->xf_spos & 0x3FFu) + soff % src->stride;
    uint32_t sy = ((s_ge->xf_spos >> 10) & 0x3FFu) + soff / src->stride;
    uint32_t dx = (s_ge->xf_dpos & 0x3FFu) + doff % dst->stride;
    uint32_t dy = ((s_ge->xf_dpos >> 10) & 0x3FFu) + doff / dst->stride;
    uint32_t w = (s_ge->xf_size & 0x3FFu) + 1, h = ((s_ge->xf_size >> 10) & 0x3FFu) + 1;
    if (sx + w > FB_W || dx + w > FB_W || sy + h > FB_H || dy + h > FB_H) return 0;
    if (!dst->gpu_valid && !target_upload(dst)) return 0;   /* seed rows we don't overwrite */
    if (s_nbatch && (s_cur == src || s_cur == dst)) submit_pending();
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
              VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to_layout(s_cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, &dst->layout,
              VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageCopy ic = {0};
    ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ic.srcSubresource.layerCount = 1;
    ic.dstSubresource = ic.srcSubresource;
    ic.srcOffset.x = (int32_t)(sx * (uint32_t)s_scale); ic.srcOffset.y = (int32_t)(sy * (uint32_t)s_scale);
    ic.dstOffset.x = (int32_t)(dx * (uint32_t)s_scale); ic.dstOffset.y = (int32_t)(dy * (uint32_t)s_scale);
    ic.extent.width = w * (uint32_t)s_scale; ic.extent.height = h * (uint32_t)s_scale; ic.extent.depth = 1;
    vkCmdCopyImage(s_cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
    to_layout(s_cmd, dst->img, VK_IMAGE_ASPECT_COLOR_BIT, &dst->layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit_wait(SR_PERF_GE_TRANSFER_BLIT)) return 0;
    dst->render_gen++;                       /* content changed (snapshot/present tracking) */
    if (g_ace_on) ace_note_target_draw(dst->fba);
    sr_vram_mark_renderer_pending();
    dst->lru = s_lru++;
    s_cnt_xferblit++;
    return 1;
}

int gegpu_capture_materialize(void) {
    if (!s_ready) return 1;
    if (!submit_pending()) return 0;
    if (!cmd_drain(GEGPU_BOUNDARY_READBACK)) return 0;
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used && s_tgts[i].gpu_valid && !target_readback(&s_tgts[i])) return 0;
    for (int i = 0; i < MAX_DEP; i++)
        if (s_deps[i].used && !depth_readback_to_guest(&s_deps[i])) return 0;
    return 1;
}

int gegpu_replay_reset(void) {
    if (!s_ready) return 1;
    if (!submit_pending()) return 0;
    if (!cmd_drain(GEGPU_BOUNDARY_LIFETIME)) return 0;
    readback_poll();
    for (int i = 0; i < READBACK_FRAMES; i++)
        if (s_readback[i].pending && readback_finish(&s_readback[i], 1, 0) < 0) return 0;
    for (int i = 0; i < MAX_TGT; i++) {
        s_tgts[i].gpu_valid = 0;
        s_tgts[i].clean_gen = 0;
    }
    for (int i = 0; i < MAX_DEP; i++) s_deps[i].cpu_dirty = 1;
    for (int i = 0; i < s_tex_n; i++) s_tex[i].content_valid = 0;
    s_cur = NULL;
    s_snap_src = NULL;
    s_snap_srcgen = 0;
    s_tmpl_ok = 0;
    return 1;
}

void gegpu_replay_stats_reset(void) {
    memset(&s_replay_stats, 0, sizeof(s_replay_stats));
    memset(&s_cpu_profile_stats, 0, sizeof(s_cpu_profile_stats));
    s_cpu_profile_hook_depth = 0;
    s_cpu_profile_stats.enabled = s_cpu_profile;
}

void gegpu_replay_stats_get(GeGpuReplayStats *out) {
    if (out) *out = s_replay_stats;
}

/* Cumulative SR_GPU_STATS replay counters for the perf CSV (perf.c samples this once per
 * interval).  Only the per-interval counts are exported; see SrPerfGpuReplayCounter. */
static void gpu_replay_snapshot(uint64_t out[SR_PERF_GPU_REPLAY_COUNT]) {
    GeGpuReplayStats s;
    gegpu_replay_stats_get(&s);
    out[SR_PERF_GPU_QUEUE_SUBMITS] = s.queue_submits;
    out[SR_PERF_GPU_BATCHES] = s.batches;
    out[SR_PERF_GPU_DRAWS] = s.draws;
    out[SR_PERF_GPU_SNAPSHOT_REQUESTS] = s.snapshot_requests;
    out[SR_PERF_GPU_SNAPSHOT_COPIES] = s.snapshot_copies;
    out[SR_PERF_GPU_UPLOAD_RING_RESERVATIONS] = s.upload_ring_reservations;
    out[SR_PERF_GPU_UPLOAD_RING_BYTES] = s.upload_ring_bytes;
    out[SR_PERF_GPU_UPLOAD_RING_WRAPS] = s.upload_ring_wraps;
    out[SR_PERF_GPU_UPLOAD_RING_FALLBACKS] = s.upload_ring_fallbacks;
    out[SR_PERF_GPU_SNAPSHOT_COPY_PX] = s.snapshot_copy_px;
    out[SR_PERF_GPU_FALLBACK_PRIMS] = s.fallback_prims;
    out[SR_PERF_GPU_STENCIL_SPLITS] = s.stencil_splits;
    for (int i = 0; i < GEGPU_FALLBACK_REASONS; i++)
        out[SR_PERF_GPU_FALLBACK_STENCIL + i] = s.fallback_readbacks[i];
}

void gegpu_cpu_profile_stats_get(GeGpuCpuProfileStats *out) {
    if (out) *out = s_cpu_profile_stats;
}

/* ---- GE sync points ------------------------------------------------------------------------ */

void gegpu_flush(const char *reason) {
    if (!s_ready) return;
    readback_poll();
    if (reason && strcmp(reason, "listend") == 0) { stats_tick(); return; }
    if (reason && strcmp(reason, "xfersrc") == 0) {
        /* GE block transfer is about to READ guest memory: materialize any GPU-resident
         * target overlapping the source rows into guest VRAM (the transfer itself runs on
         * the CPU in ge.c; the destination range is invalidated via vram_dirty after). */
        submit_pending();
        uint32_t srcBase = (s_ge->xf_src & 0xFFFFF0u) | ((s_ge->xf_srcw & 0xFF0000u) << 8);
        if (!is_vram(srcBase)) return;
        uint32_t srcStride = s_ge->xf_srcw & 0x7F8u;
        uint32_t srcY = (s_ge->xf_spos >> 10) & 0x3FFu;
        uint32_t h = ((s_ge->xf_size >> 10) & 0x3FFu) + 1;
        uint32_t a0 = vram_off(srcBase);
        uint32_t bytes = (srcY + h) * (srcStride ? srcStride : 512u) * 4u;  /* conservative */
        for (int i = 0; i < MAX_TGT; i++) {
            Target *t = &s_tgts[i];
            if (!t->used || !t->gpu_valid) continue;
            uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
            uint32_t flen = t->stride * FB_H * bpp_t;
            if (a0 < t->fba + flen && t->fba < a0 + bytes)
                target_readback(t);
        }
        return;
    }
    if (reason && strcmp(reason, "loadclut") == 0) {
        /* the CLUT loader is about to READ guest VRAM: materialize any target there */
        if (!is_vram(s_ge->clut_addr)) return;
        uint32_t ca = vram_off(s_ge->clut_addr);
        for (int i = 0; i < MAX_TGT; i++) {
            Target *t = &s_tgts[i];
            if (!t->used || !t->gpu_valid) continue;
            uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
            uint32_t flen = t->stride * FB_H * bpp_t;
            if (ca < t->fba + flen && t->fba < ca + 2048)
                target_readback(t);
        }
        return;
    }
    submit_pending();
}

/* Present: hand the GPU image straight to the swapchain. Returns 0 if this address is
 * not GPU-resident (CPU-written movie frames, pre-GPU content) — caller uses the
 * guest-VRAM path. */
int gegpu_present(uint32_t fbaddr, int fmt, uint32_t stride) {
    {
        static int dbg = -1;
        if (dbg < 0) dbg = getenv("SR_GPU_PRESENT_DBG") ? 1 : 0;
        if (dbg) fprintf(stderr, "GEGPU_PRESENT[%llu]: fbaddr=0x%08x fmt=%d stride=%u s_ready=%d\n",
            (unsigned long long)s_cnt_present_gpu + s_cnt_present_cpu, fbaddr, fmt, stride, s_ready);
    }
    if (!s_ready) return GEGPU_PRESENT_NOT_RESIDENT;
    readback_poll();
    /* Display buffers in main RAM (movie frames, CPU-composed screens) are never
     * GPU-resident; folding them into the VRAM window could alias a real target. */
    if (!is_vram(fbaddr)) { s_cnt_present_cpu++; return GEGPU_PRESENT_NOT_RESIDENT; }
    uint32_t fba = vram_off(fbaddr);
    Target *t = target_color_acquire(fba, stride ? stride : 512u, (uint32_t)fmt, 1);
    if (!t || !t->gpu_valid || (int)t->fmt != (fmt & 3)) {
        { static int n = 0; if (n < 3 || getenv("SR_GPU_PRESENT_DBG"))
            fprintf(stderr, "GEGPU_PRESENT[%llu]: EARLY-EXIT t=%p gpu_valid=%d fmt=%d/%d\n",
                (unsigned long long)s_cnt_present_gpu + s_cnt_present_cpu, (void*)t, t ? t->gpu_valid : -1,
                t ? (int)t->fmt : -1, fmt & 3); n++; }
        s_cnt_present_cpu++; return GEGPU_PRESENT_NOT_RESIDENT; }
    /* A GPU-valid target that cannot be prepared may hold the frame's newest pixels, which
     * then exist only in its image. Report it apart from the NOT_RESIDENT declines, where no
     * target owns a newer frame, so gui_present syncs guest VRAM only for this code (issue #827). */
    if (!target_prepare_present(t)) return GEGPU_PRESENT_TARGET_FAILED;
    s_cnt_present_gpu++;
    stats_tick();
    int pr = sdl3vk_present_image_ex((void *)t->img, DRAW_W * s_scale, FB_H * s_scale);
    /* Only SDL's -1 is remapped: it is a present that failed after the target was prepared, so
     * the target's newest pixels may exist only in its image. SDL's 0 (user close) and 1 (shown)
     * are GEGPU_PRESENT_WINDOW_CLOSED and GEGPU_PRESENT_SHOWN and pass through unchanged. */
    if (pr == -1) return GEGPU_PRESENT_TARGET_FAILED;
    return pr;
}

/* Explicit snapshot boundary. Ordinary presentation remains asynchronous; callers that
 * are about to publish a guest-VRAM dump use this target-scoped operation to settle only
 * the framebuffer described by the display controller. */
int gegpu_sync_guest_fb(const GeGpuFbDescriptor *desc) {
    GeGpuFbSpan span;
    const char *why = NULL;
    if (!gegpu_validate_guest_fb_descriptor(desc, &span, &why)) {
        fprintf(stderr, "gegpu: snapshot sync refused: %s (addr=0x%08x fmt=%u stride=%u %ux%u)\n",
                why ? why : "invalid descriptor",
                desc ? desc->addr : 0u, desc ? desc->format : 0u,
                desc ? desc->stride : 0u, desc ? desc->width : 0u,
                desc ? desc->height : 0u);
        return GEGPU_SYNC_FAILED;
    }
    if (!s_ready) return GEGPU_SYNC_NO_TARGET;
    if (!span.in_vram) return GEGPU_SYNC_NO_TARGET;

    Target *t = NULL;
    for (int i = 0; i < MAX_TGT; i++) {
        if (s_tgts[i].used && s_tgts[i].fba == span.vram_offset) {
            t = &s_tgts[i];
            break;
        }
    }
    if (!t) return GEGPU_SYNC_NO_TARGET;
    if (t->stride != desc->stride || t->fmt != desc->format) {
        fprintf(stderr,
                "gegpu: snapshot sync refused: target at 0x%08x has stride=%u fmt=%u "
                "but caller described stride=%u fmt=%u\n",
                desc->addr, t->stride, t->fmt, desc->stride, desc->format);
        return GEGPU_SYNC_FAILED;
    }

    unsigned long long failures_before = s_readback_commit_failures;
    readback_poll();
    if (!t->gpu_valid) {
        if (t->clean_gen == t->render_gen &&
            s_readback_commit_failures == failures_before)
            return GEGPU_SYNC_NO_TARGET;
        fprintf(stderr,
                "gegpu: snapshot sync refused: target at 0x%08x has uncommitted content "
                "(clean_gen=%llu render_gen=%llu)\n",
                desc->addr, (unsigned long long)t->clean_gen,
                (unsigned long long)t->render_gen);
        return GEGPU_SYNC_FAILED;
    }

    uint64_t want = t->render_gen;
    if (!target_readback(t)) return GEGPU_SYNC_FAILED;
    if (t->render_gen > want) want = t->render_gen;
    if (t->clean_gen != want || s_readback_commit_failures != failures_before) {
        fprintf(stderr,
                "gegpu: snapshot sync incomplete for 0x%08x clean_gen=%llu want=%llu failures=%llu\n",
                desc->addr, (unsigned long long)t->clean_gen,
                (unsigned long long)want,
                s_readback_commit_failures - failures_before);
        return GEGPU_SYNC_FAILED;
    }
    return GEGPU_SYNC_OK;
}

/* ---- init ----------------------------------------------------------------------------------- */

static const GeGpuHooks k_hooks = {
    hook_tri, hook_sprite, hook_line, hook_point, gegpu_flush, hook_vram_dirty, hook_xfer,
    gegpu_capture_materialize, hook_vram_read, hook_vram_write_prepare, hook_vram_pending,
    hook_tnl_prim,
};

#ifdef SR_GPU_COHERENCE_SELFTEST
typedef struct CoherenceCase {
    const char *name;
    uint32_t fmt, stride;
    uint32_t dirty_offset, dirty_bytes;
    int pending_batch;
    int pending_readback;
    int dirty_alias;
} CoherenceCase;

static void coherence_fill_raw(uint8_t *dst, uint32_t pixels, uint32_t bpp, uint32_t raw) {
    for (uint32_t i = 0; i < pixels; i++) memcpy(dst + i * bpp, &raw, bpp);
}

static void coherence_reset_targets(void) {
    if (s_nbatch) (void)submit_pending();
    s_cur = NULL;
    for (int i = 0; i < MAX_TGT; i++)
        if (s_tgts[i].used) target_destroy(&s_tgts[i]);
    s_tmpl_ok = 0;
}

static uint32_t coherence_g1(uint32_t fmt) {
    static const uint32_t value[4] = { 0x4a69u, 0xca69u, 0xa73cu, 0x50607080u };
    return value[fmt & 3];
}

static uint32_t coherence_g0(uint32_t fmt) {
    static const uint32_t value[4] = { 0x1357u, 0x9357u, 0x2468u, 0x10203040u };
    return value[fmt & 3];
}

static uint32_t coherence_gbatch(uint32_t fmt) {
    static const uint32_t value[4] = { 0x6e25u, 0xee25u, 0xbb55u, 0xaa55aa55u };
    return value[fmt & 3];
}

static int coherence_queue_g1_batch(uint32_t base, uint32_t stride, uint32_t fmt) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = fmt;
    s_ge->zbp = 0x04080000u;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535;
    s_ge->clear = 1;
    s_ge->clear_mode = 0x301;
    uint32_t g_batch = coherence_gbatch(fmt);
    uint32_t rgba = fb_unpack(g_batch, fmt);
    GeVtx p0 = { 10.0f, 10.0f, 1.0f, 1.0f, 0, 0, 1.0f,
                 rgba & 0xff, (rgba >> 8) & 0xff, (rgba >> 16) & 0xff, rgba >> 24 };
    GeVtx p1 = p0;
    p1.x = 20.0f;
    p1.y = 20.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_nbatch) return 0;
    return 1;
}

/* A CPU load after a completed GE list must materialize only the GPU-owned
 * surface it touches. This drives the production GeGpuHooks and recomp.h scalar
 * accessors; it does not call the explicit snapshot API. */
static int coherence_run_scalar_vram_read(uint32_t depth_base) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u;
    const uint32_t color_offset = (10u * stride + 10u) * 4u;
    const uint32_t depth_offset = (10u * stride + 10u) * 2u;
    /* The GE XORs bits 6 and 13 of the absolute address (PSP-measured absolute form). */
    const uint32_t depth_swizzled = (depth_base + depth_offset) ^ 0x2040u;
    const uint32_t depth_linear_alias = 0x04200000u +
                                        (depth_base - 0x04000000u) + depth_offset;
    coherence_reset_targets();
    g_sr_vram_read_pending = 0;
    for (int i = 0; i < MAX_DEP; i++) {
        if (s_deps[i].used) {
            s_deps[i].guest_dirty = 0;
            s_deps[i].cpu_dirty = 1;
        }
    }
    memset(SR_HOST(base), 0, stride * FB_H * 4u);
    memset(SR_HOST(depth_base), 0, 0x44000u + 0x2040u);

    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = 3u;
    s_ge->zbp = depth_base;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535u;
    s_ge->ztest_enable = 1;
    s_ge->ztest = 1u;
    s_ge->zwrite_disable = 0;
    GeVtx p0 = { 10.0f, 10.0f, (float)0x1111u, 1.0f, 0.0f, 0.0f, 1.0f,
                 0x40, 0x50, 0x60, 0xff };
    GeVtx p1 = p0;
    p1.x = 12.0f;
    p1.y = 12.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_nbatch) {
        fprintf(stderr, "gpu coherence selftest: failed to queue scalar-read sprite\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_flush("listend");
    uint32_t before = 1u;
    memcpy(&before, SR_HOST(base + color_offset), sizeof before);
    if (before != 0u || !g_sr_vram_read_pending) {
        fprintf(stderr,
                "gpu coherence selftest: list end materialized color early or missed pending ownership\n");
        coherence_reset_targets();
        return 0;
    }

    uint64_t lazy_color_start = SDL_GetTicksNS();
    uint32_t color = MEM_R32(base + color_offset);
    uint64_t lazy_color_ns = SDL_GetTicksNS() - lazy_color_start;
    if (color != 0x00605040u) {
        fprintf(stderr,
                "gpu coherence selftest: scalar framebuffer load (depth base 0x%08x) returned 0x%08x, expected 0x00605040\n",
                depth_base, color);
        coherence_reset_targets();
        return 0;
    }
    uint64_t lazy_depth_start = SDL_GetTicksNS();
    uint16_t depth = MEM_R16(depth_swizzled);
    uint64_t lazy_depth_ns = SDL_GetTicksNS() - lazy_depth_start;
    uint16_t depth_linear = MEM_R16(depth_linear_alias);
    if (depth != 0x1111u || depth_linear != 0x1111u) {
        fprintf(stderr,
                "gpu coherence selftest: depth scalar loads returned base=%04x mirror=%04x, expected 1111\n",
                depth, depth_linear);
        coherence_reset_targets();
        return 0;
    }

    enum { ACCESSOR_BENCH_ITERATIONS = 500000u };
    volatile uint32_t accessor_sink = 0u;
    uint64_t check_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++) {
        sr_guest_vram_read_prepare_bytes(0x08010000u + ((i & 1023u) * 4u), 4u);
        accessor_sink += i + 1u;
    }
    uint64_t check_ns = SDL_GetTicksNS() - check_start;
    uint64_t loop_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++) accessor_sink += i + 1u;
    uint64_t loop_ns = SDL_GetTicksNS() - loop_start;
    uint64_t scalar_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++)
        accessor_sink += MEM_R32(0x08010000u + ((i & 1023u) * 4u));
    uint64_t scalar_ns = SDL_GetTicksNS() - scalar_start;
    uint64_t raw_start = SDL_GetTicksNS();
    for (uint32_t i = 0; i < ACCESSOR_BENCH_ITERATIONS; i++) {
        uint32_t value;
        memcpy(&value, SR_HOST_LINEAR(0x08010000u + ((i & 1023u) * 4u)), sizeof value);
        accessor_sink += value;
    }
    uint64_t raw_ns = SDL_GetTicksNS() - raw_start;
    (void)accessor_sink;

    MEM_W16(depth_swizzled, 0xFFFFu);
    MEM_W32(base + color_offset, 0xFF112233u);
    s_ge->ztest = 4u;                    /* less: the CPU depth store must make this pass */
    s_ge->blend_enable = 1;
    s_ge->blend_mode = 3u | (10u << 4); /* source alpha zero, destination fixed B */
    s_ge->blend_fixb = 0x00FFFFFFu;
    GeVtx q0 = { 10.0f, 10.0f, (float)0x4444u, 1.0f, 0.0f, 0.0f, 1.0f,
                 0x01, 0x02, 0x03, 0xff };
    GeVtx q1 = q0;
    q1.x = 12.0f;
    q1.y = 12.0f;
    if (!hook_sprite(&q0, &q1, 0)) {
        fprintf(stderr, "gpu coherence selftest: CPU-written targets were not drawable\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t hybrid_submit_start = SDL_GetTicksNS();
    if (!submit_pending()) {
        fprintf(stderr, "gpu coherence selftest: CPU-written target batch did not submit\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t hybrid_submit_ns = SDL_GetTicksNS() - hybrid_submit_start;
    uint64_t hybrid_color_start = SDL_GetTicksNS();
    uint32_t color_after_cpu_write = MEM_R32(base + color_offset);
    uint64_t hybrid_color_ns = SDL_GetTicksNS() - hybrid_color_start;
    uint64_t hybrid_depth_start = SDL_GetTicksNS();
    uint16_t depth_after_cpu_write = MEM_R16(depth_swizzled);
    uint64_t hybrid_depth_ns = SDL_GetTicksNS() - hybrid_depth_start;
    if (color_after_cpu_write != 0xFF112233u || depth_after_cpu_write != 0x4444u) {
        fprintf(stderr,
                "gpu coherence selftest: post-store draw returned color=%08x depth=%04x, expected ff112233/4444\n",
                color_after_cpu_write, depth_after_cpu_write);
        coherence_reset_targets();
        return 0;
    }
    puts("gpu coherence selftest: PASS cpu_stores_consumed_by_next_gpu_draw");

    s_ge->ztest = 1u;
    s_ge->blend_enable = 0;
    GeVtx e0 = { 14.0f, 14.0f, (float)0x5555u, 1.0f, 0.0f, 0.0f, 1.0f,
                 0x70, 0x60, 0x50, 0xff };
    GeVtx e1 = e0;
    e1.x = 16.0f;
    e1.y = 16.0f;
    if (!hook_sprite(&e0, &e1, 0)) {
        fprintf(stderr, "gpu coherence selftest: could not queue eager-sync candidate draw\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t eager_submit_start = SDL_GetTicksNS();
    if (!submit_pending()) {
        fprintf(stderr, "gpu coherence selftest: eager-sync candidate did not submit\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t eager_submit_ns = SDL_GetTicksNS() - eager_submit_start;
    uint64_t eager_materialize_start = SDL_GetTicksNS();
    if (!target_readback(s_cur) || !depth_readback_to_guest(s_cur->dep)) {
        fprintf(stderr, "gpu coherence selftest: eager-sync candidate materialization failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint64_t eager_materialize_ns = SDL_GetTicksNS() - eager_materialize_start;
    printf("gpu coherence selftest: VRAM timing ns lazy-first-touch color=%" PRIu64
           " depth=%" PRIu64 " hybrid-listend-submit=%" PRIu64
           " hybrid-first-touch color=%" PRIu64 " depth=%" PRIu64
           " eager-listend-submit=%" PRIu64 " eager-sync-materialize=%" PRIu64
           " pending-check-extra=%" PRIu64 "/%u calls scalar-load=%" PRIu64
           " raw-load=%" PRIu64 " scalar-extra=%" PRIu64 "\n",
           lazy_color_ns, lazy_depth_ns, hybrid_submit_ns,
           hybrid_color_ns, hybrid_depth_ns, eager_submit_ns,
           eager_materialize_ns,
           check_ns > loop_ns ? (check_ns - loop_ns) / ACCESSOR_BENCH_ITERATIONS : 0u,
           ACCESSOR_BENCH_ITERATIONS, scalar_ns / ACCESSOR_BENCH_ITERATIONS,
           raw_ns / ACCESSOR_BENCH_ITERATIONS,
           scalar_ns > raw_ns ? (scalar_ns - raw_ns) / ACCESSOR_BENCH_ITERATIONS : 0u);
    puts("gpu coherence selftest: PASS scalar_cpu_read color+depth after list-end");
    coherence_reset_targets();
    return 1;
}

/* Establish issue 145's split generations without duplicating coherence: production
 * target_upload() creates GPU G1, the harness restores stale guest G0, then a real CPU
 * byte write flows through sr_gpu_vram_dirty(), production reacquire, and readback. */
static int coherence_run_case(const CoherenceCase *tc) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    uint32_t bpp = tc->fmt == 3 ? 4u : 2u;
    uint32_t pixels = tc->stride * FB_H;
    uint32_t target_bytes = pixels * bpp;
    if (!tc->dirty_bytes || tc->dirty_offset + tc->dirty_bytes > target_bytes) return 0;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    uint8_t *payload = (uint8_t *)malloc(tc->dirty_bytes);
    if (!expected || !payload) { free(expected); free(payload); return 0; }
    uint32_t g1 = coherence_g1(tc->fmt);
    coherence_fill_raw(guest, pixels, bpp, g1);
    coherence_fill_raw(expected, pixels, bpp, g1);

    Target *t = target_color_acquire(fba, tc->stride, tc->fmt, 1);
    if (!t || !t->gpu_valid) {
        fprintf(stderr, "gpu coherence selftest [%s]: could not establish GPU G1\n", tc->name);
        free(expected); free(payload); return 0;
    }
    coherence_fill_raw(guest, pixels, bpp, coherence_g0(tc->fmt));
    t->clean_gen = 0;

    if (tc->pending_batch) {
        if (!coherence_queue_g1_batch(base, tc->stride, tc->fmt)) {
            fprintf(stderr, "gpu coherence selftest [%s]: could not queue production batch\n", tc->name);
            free(expected); free(payload); return 0;
        }
        uint32_t g_batch = coherence_gbatch(tc->fmt);
        for (uint32_t py = 10; py < 20; py++) {
            for (uint32_t px = 10; px < 20; px++) {
                uint32_t b_offset = (py * tc->stride + px) * bpp;
                memcpy(expected + b_offset, &g_batch, bpp);
            }
        }
    }

    ReadbackSlot *stale = NULL;
    if (tc->pending_readback) {
        if (!target_prepare_present(t)) {
            fprintf(stderr, "gpu coherence selftest [%s]: async readback setup failed\n", tc->name);
            free(expected); free(payload); return 0;
        }
        for (int i = 0; i < READBACK_FRAMES; i++)
            if (s_readback[i].pending && s_readback[i].image == t->img) {
                stale = &s_readback[i]; break;
            }
        if (!stale) {
            fprintf(stderr, "gpu coherence selftest [%s]: no pending async slot\n", tc->name);
            free(expected); free(payload); return 0;
        }
    }

    for (uint32_t i = 0; i < tc->dirty_bytes; i++) payload[i] = (uint8_t)(0xd1u + i * 37u);
    memcpy(guest + tc->dirty_offset, payload, tc->dirty_bytes);
    memcpy(expected + tc->dirty_offset, payload, tc->dirty_bytes);
    uint32_t dirty_addr = base + tc->dirty_offset;
    if (tc->dirty_alias) dirty_addr += 0x40000000u;
    sr_gpu_vram_dirty(dirty_addr, tc->dirty_bytes);

    if (!t->cpu_n) {
        fprintf(stderr, "gpu coherence selftest [%s]: dirty hook did not hold the write\n", tc->name);
        free(expected); free(payload); return 0;
    }
    if (stale) {
        if (stale->commit || readback_finish(stale, 1, 1) < 0 ||
            memcmp(guest + tc->dirty_offset, payload, tc->dirty_bytes) != 0) {
            fprintf(stderr, "gpu coherence selftest [%s]: stale async completion overwrote G2\n",
                    tc->name);
            free(expected); free(payload); return 0;
        }
    }

    t = target_color_acquire(fba, tc->stride, tc->fmt, 1);
    if (!t || !target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [%s]: production reacquire/readback failed\n",
                tc->name);
        free(expected); free(payload); return 0;
    }

    size_t mismatch = 0;
    while (mismatch < target_bytes && guest[mismatch] == expected[mismatch]) mismatch++;
    if (mismatch != target_bytes) {
        fprintf(stderr,
                "gpu coherence selftest [%s]: byte %zu expected=%02x actual=%02x\n",
                tc->name, mismatch, expected[mismatch], guest[mismatch]);
        free(expected); free(payload); return 0;
    }
    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           tc->name, tc->fmt, tc->stride, s_scale);
    free(expected);
    free(payload);
    return 1;
}

static int coherence_run_clean_case(const char *name, uint32_t fmt, uint32_t stride,
                                     uint32_t dirty_offset, uint32_t dirty_bytes) {
    const uint32_t fba = 0x00120000u;
    const uint32_t base = 0x04000000u | fba;
    uint32_t bpp = fmt == 3 ? 4u : 2u;
    uint32_t pixels = stride * FB_H;
    uint32_t target_bytes = pixels * bpp;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    uint8_t *payload = (uint8_t *)malloc(dirty_bytes);
    if (!expected || !payload) { free(expected); free(payload); return 0; }

    uint32_t g1 = coherence_g1(fmt);
    coherence_fill_raw(guest, pixels, bpp, g1);
    coherence_fill_raw(expected, pixels, bpp, g1);

    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid || t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu coherence selftest [%s]: clean_gen initial setup failed\n", name);
        free(expected); free(payload); return 0;
    }

    for (uint32_t i = 0; i < dirty_bytes; i++) payload[i] = (uint8_t)(0xb5u + i * 19u);
    memcpy(guest + dirty_offset, payload, dirty_bytes);
    memcpy(expected + dirty_offset, payload, dirty_bytes);

    sr_gpu_vram_dirty(base + dirty_offset, dirty_bytes);

    if (t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu coherence selftest [%s]: clean_gen did not advance to render_gen\n", name);
        free(expected); free(payload); return 0;
    }

    uint64_t pre_rb = s_cnt_readback;
    if (!target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [%s]: target_readback failed\n", name);
        free(expected); free(payload); return 0;
    }
    if (s_cnt_readback != pre_rb) {
        fprintf(stderr, "gpu coherence selftest [%s]: readback performed unnecessary copy for clean target\n", name);
        free(expected); free(payload); return 0;
    }

    size_t mismatch = 0;
    while (mismatch < target_bytes && guest[mismatch] == expected[mismatch]) mismatch++;
    if (mismatch != target_bytes) {
        fprintf(stderr, "gpu coherence selftest [%s]: guest VRAM mismatch at byte %zu\n", name, mismatch);
        free(expected); free(payload); return 0;
    }

    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           name, fmt, stride, s_scale);
    free(expected); free(payload);
    return 1;
}

/* Many small CPU writes into a resident target between two GE uses are held and applied
 * together (target_note_cpu_write, target_sync_cpu_writes), not one image patch each. The GPU
 * holds G1 while guest VRAM holds G0, so a byte the image takes from the wrong side shows.
 * 1024 adjacent 4-byte stores fill two rows and merge into one span; 48 unaligned 2-byte
 * stores in other rows split pixels and outnumber the spans, so one group is applied early.
 * The readback must hold the written bytes, G1 everywhere else (the GPU side of every split
 * pixel included), with far fewer patches than writes. */
static int coherence_run_cpu_write_coalesce(void) {
    const char *name = "cpu-write-coalesce";
    const uint32_t fba = 0x00150000u, base = 0x04000000u | fba;
    const uint32_t fmt = 3u, stride = 512u, bpp = 4u, pixels = stride * FB_H;
    const uint32_t target_bytes = pixels * bpp;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    if (!expected) return 0;
    coherence_fill_raw(guest, pixels, bpp, coherence_g1(fmt));
    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid) { free(expected); return 0; }
    memcpy(expected, guest, target_bytes);              /* the image: G1 */
    coherence_fill_raw(guest, pixels, bpp, coherence_g0(fmt));
    t->clean_gen = 0;                                   /* the image is newer than VRAM */

    const unsigned long long notes0 = s_replay_stats.cpu_write_notes;
    const unsigned long long syncs0 = s_replay_stats.cpu_write_syncs;
    const unsigned uploads0 = s_cnt_upload;
    for (uint32_t i = 0; i < 1024u; i++) {             /* rows 40 and 41, every pixel */
        const uint32_t off = (40u * stride) * bpp + i * 4u;
        for (uint32_t k = 0; k < 4u; k++) guest[off + k] = expected[off + k] = (uint8_t)(0x31u + i + k);
        sr_gpu_vram_dirty(base + off, 4u);
    }
    for (uint32_t i = 0; i < 48u; i++) {               /* one 2-byte store per row, mid-pixel */
        const uint32_t off = ((100u + i) * stride + 7u * i) * bpp + 1u;
        guest[off] = expected[off] = (uint8_t)(0xa0u + i);
        guest[off + 1u] = expected[off + 1u] = (uint8_t)(0x50u + i);
        sr_gpu_vram_dirty(base + off, 2u);
    }
    const unsigned long long notes = s_replay_stats.cpu_write_notes - notes0;
    if (!target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [%s]: readback failed\n", name);
        free(expected); return 0;
    }
    const unsigned long long syncs = s_replay_stats.cpu_write_syncs - syncs0;
    const unsigned uploads = s_cnt_upload - uploads0;
    size_t mismatch = 0;
    while (mismatch < target_bytes && guest[mismatch] == expected[mismatch]) mismatch++;
    if (mismatch != target_bytes) {
        fprintf(stderr, "gpu coherence selftest [%s]: byte %zu expected=%02x actual=%02x\n",
                name, mismatch, expected[mismatch], guest[mismatch]);
        free(expected); return 0;
    }
    if (notes != 1072u || syncs != 2u || uploads > 1u + 48u) {
        fprintf(stderr, "gpu coherence selftest [%s]: notes=%llu syncs=%llu uploads=%u\n",
                name, notes, syncs, uploads);
        free(expected); return 0;
    }
    printf("gpu coherence selftest: PASS %-22s writes=%llu groups=%llu patches=%u\n",
           name, notes, syncs, uploads);
    coherence_reset_targets();
    free(expected);
    return 1;
}

static int coherence_run_full_cover_case(void) {
    const char *name = "full-cover-stale";
    const uint32_t fba = 0x00130000u;
    const uint32_t base = 0x04000000u | fba;
    uint32_t fmt = 3, stride = 512, bpp = 4;
    uint32_t pixels = stride * FB_H;
    uint32_t target_bytes = pixels * bpp;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    uint8_t *expected = (uint8_t *)malloc(target_bytes);
    if (!expected) return 0;

    uint32_t g1 = coherence_g1(fmt);
    coherence_fill_raw(guest, pixels, bpp, g1);
    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid) { free(expected); return 0; }

    t->clean_gen = 0;

    for (uint32_t i = 0; i < target_bytes; i++) {
        guest[i] = (uint8_t)(0x77u + i * 13u);
        expected[i] = guest[i];
    }
    sr_gpu_vram_dirty(base, target_bytes);

    if (!target_sync_cpu_writes(t) || t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu coherence selftest [%s]: full cover write failed to advance clean_gen\n", name);
        free(expected); return 0;
    }

    if (!target_readback(t) || memcmp(guest, expected, target_bytes) != 0) {
        fprintf(stderr, "gpu coherence selftest [%s]: target readback verification failed\n", name);
        free(expected); return 0;
    }

    printf("gpu coherence selftest: PASS %-22s fmt=%u stride=%u scale=%d\n",
           name, fmt, stride, s_scale);
    free(expected);
    return 1;
}

static int coherence_run_overlap_case(void) {
    const uint32_t fba_a = 0x000f0000u, fba_b = 0x00110000u;
    const uint32_t base_a = 0x04000000u | fba_a, base_b = 0x04000000u | fba_b;
    const uint32_t stride = FB_W, bpp = 4, bytes = stride * FB_H * bpp;
    const uint32_t g1a = 0x31527394u, g1b = 0x426384a5u, g0 = 0x10203040u;
    const uint32_t dirty_addr = base_b + 123u;
    const uint8_t payload[7] = { 0xe1, 0x17, 0xc3, 0x49, 0xa5, 0x6b, 0x8d };
    coherence_reset_targets();
    uint8_t *ga = (uint8_t *)SR_HOST(base_a), *gb = (uint8_t *)SR_HOST(base_b);
    uint8_t *ea = (uint8_t *)malloc(bytes), *eb = (uint8_t *)malloc(bytes);
    if (!ea || !eb) { free(ea); free(eb); return 0; }

    coherence_fill_raw(ga, stride * FB_H, bpp, g1a);
    Target *a = target_color_acquire(fba_a, stride, 3, 1);
    coherence_fill_raw(gb, stride * FB_H, bpp, g1b);
    Target *b = target_color_acquire(fba_b, stride, 3, 1);
    if (!a || !b) { free(ea); free(eb); return 0; }
    coherence_fill_raw(ga, (bytes + (fba_b - fba_a)) / bpp, bpp, g0);
    coherence_fill_raw(ea, stride * FB_H, bpp, g1a);
    coherence_fill_raw(eb, stride * FB_H, bpp, g1b);
    a->clean_gen = b->clean_gen = 0;
    memcpy(SR_HOST(dirty_addr), payload, sizeof(payload));
    memcpy(ea + (dirty_addr - base_a), payload, sizeof(payload));
    memcpy(eb + (dirty_addr - base_b), payload, sizeof(payload));
    sr_gpu_vram_dirty(dirty_addr, sizeof(payload));

    int ok = a->gpu_valid && b->gpu_valid && target_readback(a) &&
             memcmp(SR_HOST(base_a), ea, bytes) == 0 && target_readback(b) &&
             memcmp(SR_HOST(base_b), eb, bytes) == 0;
    if (!ok) fprintf(stderr, "gpu coherence selftest [overlapping-targets]: preservation failed\n");
    else printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
                "overlapping-targets", stride, s_scale);
    free(ea); free(eb);
    return ok;
}

static int coherence_run_tflt_descriptor_capacity(void) {
    const uint32_t stride = FB_W;
    coherence_reset_targets();
    for (unsigned i = 0; i < MAX_TGT; i++) {
        uint32_t fba = 0x00020000u + i * 0x00010000u;
        Target *target = target_color_acquire(fba, stride, 3u, 1);
        if (!target) {
            fprintf(stderr,
                    "gpu coherence selftest [TFLT-descriptors]: target %u/%u exhausted the fixed sampler pool\n",
                    i + 1u, MAX_TGT);
            coherence_reset_targets();
            return 0;
        }
        for (unsigned filter = 0; filter < TEX_FILTER_VARIANTS; filter++) {
            if (!target->set[filter]) {
                fprintf(stderr,
                        "gpu coherence selftest [TFLT-descriptors]: target %u lacks sampler variant %u\n",
                        i, filter);
                coherence_reset_targets();
                return 0;
            }
        }
    }
    coherence_reset_targets();
    printf("gpu coherence selftest: PASS TFLT descriptor-capacity targets=%u variants=%u sets=%u\n",
           MAX_TGT, TEX_FILTER_VARIANTS, FIXED_FILTER_DESCRIPTOR_SETS);
    return 1;
}

typedef struct SpriteEndpoint {
    float x, y, u, v;
} SpriteEndpoint;

typedef struct SpriteExpected {
    const char *name;
    SpriteEndpoint p0, p1;
    int x, y, w, h;
    int u, v, du, dv;
} SpriteExpected;

typedef struct RawSpriteVtx {
    float u, v;
    float x, y, z;
} RawSpriteVtx;

/* Synthetic through-mode sprites.  The expected table is deliberately independent of
 * the interpolation code: it names the complete texel sequence from the screen-space
 * minimum corner. UV is linear between the vertex on each edge and every pixel samples
 * it at its centre (PSP-GE-SAMPLE-001), so a pixel shows texel floor(u at x + 0.5):
 * reverse-x-scaled's first pixel reads 8 - 0.5 * 2 = 7. */
static const SpriteExpected k_sprite_expected[] = {
    { "normal",               { 10.0f,  10.0f, 0.0f, 0.0f }, { 14.0f,  14.0f, 4.0f, 4.0f },
      10,  10, 4, 4, 0, 0,  1,  1 },
    { "reverse-x",            { 14.0f,  20.0f, 0.0f, 0.0f }, { 10.0f,  24.0f, 4.0f, 4.0f },
      10,  20, 4, 4, 3, 0, -1,  1 },
    { "reverse-y",            { 10.0f,  34.0f, 0.0f, 0.0f }, { 14.0f,  30.0f, 4.0f, 4.0f },
      10,  30, 4, 4, 0, 3,  1, -1 },
    { "reverse-xy",           { 14.0f,  44.0f, 0.0f, 0.0f }, { 10.0f,  40.0f, 4.0f, 4.0f },
      10,  40, 4, 4, 3, 3, -1, -1 },
    { "one-pixel",            { 20.0f,  50.0f, 2.0f, 2.0f }, { 21.0f,  51.0f, 3.0f, 3.0f },
      20,  50, 1, 1, 2, 2,  0,  0 },
    { "one-pixel-reverse-x",  { 21.0f,  52.0f, 2.0f, 2.0f }, { 20.0f,  53.0f, 3.0f, 3.0f },
      20,  52, 1, 1, 2, 2,  0,  0 },
    { "reverse-x-offset",     { 14.0f,  60.0f, 2.0f, 3.0f }, { 10.0f,  64.0f, 6.0f, 7.0f },
      10,  60, 4, 4, 5, 3, -1,  1 },
    { "reverse-x-scaled",     { 14.0f,  70.0f, 0.0f, 0.0f }, { 10.0f,  74.0f, 8.0f, 4.0f },
      10,  70, 4, 4, 7, 0, -2,  1 },
    { "reverse-x-subpixel",   { 14.75f, 80.75f, 0.0f, 0.0f }, { 10.75f, 84.75f, 4.0f, 4.0f },
      10,  80, 4, 4, 3, 0, -1,  1 },
    { "reverse-x-far-edge",   { 38.0f,  90.0f, 0.0f, 0.0f }, { 30.0f,  92.0f, 8.0f, 2.0f },
      30,  90, 8, 2, 7, 0, -1,  1 },
    { "one-high-reverse-y",   { 40.0f, 101.0f, 1.0f, 2.0f }, { 44.0f, 100.0f, 5.0f, 3.0f },
      40, 100, 4, 1, 1, 2,  1,  0 },
    { "one-pixel-reverse-xy", { 51.0f, 111.0f, 2.0f, 2.0f }, { 50.0f, 110.0f, 3.0f, 3.0f },
      50, 110, 1, 1, 2, 2,  0,  0 },
};

static uint32_t *s_sprite_gpu_pixels;
static size_t s_sprite_gpu_bytes;

static uint32_t coherence_sprite_texel(int u, int v) {
    uint32_t r = (uint32_t)(u + 1) * 25u;
    uint32_t g = (uint32_t)(v + 1) * 25u;
    /* Default GE alpha write state keeps the destination alpha byte at zero. */
    return 0x00640000u | (g << 8) | r;
}

static int coherence_verify_sprite_output(const char *path, const uint32_t *fb,
                                          uint32_t stride) {
    for (size_t i = 0; i < sizeof(k_sprite_expected) / sizeof(k_sprite_expected[0]); i++) {
        const SpriteExpected *tc = &k_sprite_expected[i];
        for (int dy = 0; dy < tc->h; dy++) {
            for (int dx = 0; dx < tc->w; dx++) {
                int u = tc->u + dx * tc->du;
                int v = tc->v + dy * tc->dv;
                uint32_t want = coherence_sprite_texel(u, v);
                uint32_t got = fb[(tc->y + dy) * stride + tc->x + dx];
                if (got != want) {
                    fprintf(stderr,
                            "gpu coherence selftest [sprite-%s/%s]: pixel=(%d,%d) "
                            "texel=(%d,%d) expected=%08x actual=%08x\n",
                            path, tc->name, tc->x + dx, tc->y + dy, u, v, want, got);
                    return 0;
                }
            }
        }
        /* Prove the rectangle is endpoint-exclusive on all four sides. */
        for (int dx = 0; dx < tc->w; dx++) {
            if (fb[(tc->y - 1) * stride + tc->x + dx] != 0 ||
                fb[(tc->y + tc->h) * stride + tc->x + dx] != 0) {
                fprintf(stderr, "gpu coherence selftest [sprite-%s/%s]: y edge leaked\n",
                        path, tc->name);
                return 0;
            }
        }
        for (int dy = 0; dy < tc->h; dy++) {
            if (fb[(tc->y + dy) * stride + tc->x - 1] != 0 ||
                fb[(tc->y + dy) * stride + tc->x + tc->w] != 0) {
                fprintf(stderr, "gpu coherence selftest [sprite-%s/%s]: x edge leaked\n",
                        path, tc->name);
                return 0;
            }
        }
    }
    return 1;
}

static uint32_t coherence_prepare_sprite_fixture(uint32_t base, uint32_t tex_base,
                                                 uint32_t stride) {
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);
    for (int v = 0; v < 8; v++) {
        for (int u = 0; u < 8; u++) {
            uint32_t r = (uint32_t)(u + 1) * 25u;
            uint32_t g = (uint32_t)(v + 1) * 25u;
            tex[v * 8 + u] = 0xff640000u | (g << 8) | r;
        }
    }
    sr_gpu_vram_dirty(tex_base, 8u * 8u * 4u);

    const uint32_t vaddr = 0x00101000u;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    uint32_t sprite_count = (uint32_t)(sizeof(k_sprite_expected) /
                                       sizeof(k_sprite_expected[0]));
    for (uint32_t i = 0; i < sprite_count; i++) {
        const SpriteExpected *tc = &k_sprite_expected[i];
        vtx[i * 2] = (RawSpriteVtx){ tc->p0.u, tc->p0.v, tc->p0.x, tc->p0.y, 0.0f };
        vtx[i * 2 + 1] =
            (RawSpriteVtx){ tc->p1.u, tc->p1.v, tc->p1.x, tc->p1.y, 0.0f };
    }

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x9C, base & 0x00FFFFFFu);
    DL_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 8 | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (3 << 8) | 3);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, 0);
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3 | (1 << 8));
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3 << 0) | (3 << 7) | (1 << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, (6 << 16) | (sprite_count * 2u));
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

static int coherence_run_sprite_semantics(void) {
    const uint32_t fba = 0x00160000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);

    uint64_t spr_before = s_cnt_spr;
    size_t sprite_count = sizeof(k_sprite_expected) / sizeof(k_sprite_expected[0]);
    uint32_t list_addr = coherence_prepare_sprite_fixture(base, tex_base, stride);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);

    if (s_cnt_spr != spr_before + sprite_count) {
        fprintf(stderr, "gpu coherence selftest [sprite-semantics]: hook_sprite call count mismatch\n");
        return 0;
    }

    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride, .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [sprite-semantics]: gegpu_sync_guest_fb failed\n");
        return 0;
    }

    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_sprite_output("gpu", fb, stride)) return 0;

    s_sprite_gpu_bytes = stride * FB_H * sizeof(*fb);
    free(s_sprite_gpu_pixels);
    s_sprite_gpu_pixels = (uint32_t *)malloc(s_sprite_gpu_bytes);
    if (!s_sprite_gpu_pixels) {
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    memcpy(s_sprite_gpu_pixels, fb, s_sprite_gpu_bytes);

    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "sprite-exact-gpu", stride, s_scale);
    return 1;
}

static int coherence_run_software_sprite_semantics(void) {
    const uint32_t fba = 0x00160000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;

    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);

    uint32_t list_addr = coherence_prepare_sprite_fixture(base, tex_base, stride);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    ge_set_gpu_hooks(&k_hooks);

    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_sprite_output("software", fb, stride)) {
        free(s_sprite_gpu_pixels);
        s_sprite_gpu_pixels = NULL;
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    size_t bytes = stride * FB_H * sizeof(*fb);
    if (!s_sprite_gpu_pixels || s_sprite_gpu_bytes != bytes) {
        fprintf(stderr, "gpu coherence selftest [sprite-parity]: missing GPU reference\n");
        free(s_sprite_gpu_pixels);
        s_sprite_gpu_pixels = NULL;
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    size_t mismatch = 0;
    while (mismatch < bytes && ((const uint8_t *)s_sprite_gpu_pixels)[mismatch] == guest[mismatch])
        mismatch++;
    if (mismatch != bytes) {
        fprintf(stderr,
                "gpu coherence selftest [sprite-parity]: byte=%zu gpu=%02x software=%02x\n",
                mismatch, ((const uint8_t *)s_sprite_gpu_pixels)[mismatch], guest[mismatch]);
        free(s_sprite_gpu_pixels);
        s_sprite_gpu_pixels = NULL;
        s_sprite_gpu_bytes = 0;
        return 0;
    }
    free(s_sprite_gpu_pixels);
    s_sprite_gpu_pixels = NULL;
    s_sprite_gpu_bytes = 0;

    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "sprite-exact-software", stride, s_scale);
    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "sprite-sw-gpu-parity", stride, s_scale);
    return 1;
}

/* ---- linearly filtered sprite: nothing from outside the sampled region may bleed ----
 *
 * The sprite-exact fixtures above run with GE_TEXFILTER=0 (NEAREST), so they pin the
 * endpoint-exclusive rectangle but say nothing about the filter window. This fixture
 * samples texel rows 1..4 of a texture whose row 0 is pure white, and demands that a
 * linearly filtered 1:1 sprite reproduce rows 1..4 exactly: an integer texel coordinate
 * names that texel's centre, so the filter weight is 0 and the outside row cannot
 * contribute. A half-texel window error shows up here as a white 50/50 wash on the
 * sprite's first row and column -- the one-pixel line this fixture exists to catch.
 * Both backends run the same list and the results must also agree byte for byte. */
#define COH_LIN_RECT_X 100
#define COH_LIN_RECT_Y 100
#define COH_LIN_RECT_W 4
#define COH_LIN_RECT_H 4
#define COH_LIN_TEX_U0 1u
#define COH_LIN_TEX_V0 1u
#define COH_LIN_OUTSIDE_ROW 0xFFFFFFFFu   /* pure white: no sampled texel uses 255 */

static uint32_t coherence_linear_texel(unsigned u, unsigned v) {
    if (v == 0u) return COH_LIN_OUTSIDE_ROW;          /* outside the sampled region */
    if (v > 4u) return 0xFF000000u;                    /* below it */
    return 0xFF000000u | (0x40u << 16) | ((v * 40u) << 8) | (u * 40u);
}

/* The default GE alpha-write state leaves the destination alpha byte at zero (see
 * coherence_sprite_texel), so the RGB triple is what this fixture compares. */
#define COH_LIN_RGB 0x00FFFFFFu

static uint32_t coherence_prepare_linear_sprite_fixture(uint32_t tex_base) {
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);
    for (unsigned v = 0; v < 8u; v++)
        for (unsigned u = 0; u < 8u; u++)
            tex[v * 8u + u] = coherence_linear_texel(u, v);
    sr_gpu_vram_dirty(tex_base, 8u * 8u * 4u);

    const uint32_t vaddr = 0x00101000u;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    vtx[0] = (RawSpriteVtx){ (float)COH_LIN_TEX_U0, (float)COH_LIN_TEX_V0,
                             (float)COH_LIN_RECT_X, (float)COH_LIN_RECT_Y, 0.0f };
    vtx[1] = (RawSpriteVtx){ (float)(COH_LIN_TEX_U0 + COH_LIN_RECT_W),
                             (float)(COH_LIN_TEX_V0 + COH_LIN_RECT_H),
                             (float)(COH_LIN_RECT_X + COH_LIN_RECT_W),
                             (float)(COH_LIN_RECT_Y + COH_LIN_RECT_H), 0.0f };

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 8 | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (3 << 8) | 3);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, 1);   /* GE_TEXFILTER = LINEAR: the filter window is what is under test */
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3 | (1 << 8));
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3 << 0) | (3 << 7) | (1 << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, (6 << 16) | 2u);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

static int coherence_verify_linear_sprite(const char *path, const uint32_t *fb,
                                          uint32_t stride) {
    for (int dy = 0; dy < COH_LIN_RECT_H; dy++) {
        for (int dx = 0; dx < COH_LIN_RECT_W; dx++) {
            uint32_t want = coherence_linear_texel(COH_LIN_TEX_U0 + (unsigned)dx,
                                                   COH_LIN_TEX_V0 + (unsigned)dy) & COH_LIN_RGB;
            uint32_t got = fb[(COH_LIN_RECT_Y + dy) * stride + COH_LIN_RECT_X + dx] & COH_LIN_RGB;
            if (got != want) {
                fprintf(stderr,
                        "gpu coherence selftest [linear-%s]: pixel=(%d,%d) texel=(%u,%u) "
                        "expected=%06x actual=%06x%s\n",
                        path, COH_LIN_RECT_X + dx, COH_LIN_RECT_Y + dy,
                        COH_LIN_TEX_U0 + (unsigned)dx, COH_LIN_TEX_V0 + (unsigned)dy,
                        want, got,
                        (got & COH_LIN_RGB) == (COH_LIN_OUTSIDE_ROW & COH_LIN_RGB)
                            ? " -- the texel row OUTSIDE the sampled region bled in" : "");
                return 0;
            }
        }
    }
    /* The rectangle stays endpoint-exclusive: a filter that reached outward would also
     * paint the neighbouring rows. */
    for (int dx = 0; dx < COH_LIN_RECT_W; dx++) {
        if (fb[(COH_LIN_RECT_Y - 1) * stride + COH_LIN_RECT_X + dx] != 0 ||
            fb[(COH_LIN_RECT_Y + COH_LIN_RECT_H) * stride + COH_LIN_RECT_X + dx] != 0) {
            fprintf(stderr, "gpu coherence selftest [linear-%s]: y edge leaked\n", path);
            return 0;
        }
    }
    return 1;
}

static int coherence_run_linear_sprite(void) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;

    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);

    uint32_t list_addr = coherence_prepare_linear_sprite_fixture(tex_base);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);

    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride, .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [linear-gpu]: gegpu_sync_guest_fb failed\n");
        return 0;
    }
    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_linear_sprite("gpu", fb, stride)) return 0;

    size_t bytes = stride * FB_H * sizeof(*fb);
    uint8_t *reference = (uint8_t *)malloc(bytes);
    if (!reference) {
        fprintf(stderr, "gpu coherence selftest [linear-gpu]: no reference buffer\n");
        return 0;
    }
    memcpy(reference, guest, bytes);

    memset(guest, 0, stride * FB_H * 4u);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (!coherence_verify_linear_sprite("software", fb, stride)) { free(reference); return 0; }
    size_t mismatch = 0;
    while (mismatch < bytes && reference[mismatch] == guest[mismatch]) mismatch++;
    if (mismatch != bytes) {
        fprintf(stderr, "gpu coherence selftest [linear-parity]: byte=%zu gpu=%02x software=%02x\n",
                mismatch, reference[mismatch], guest[mismatch]);
        free(reference);
        return 0;
    }
    free(reference);

    printf("gpu coherence selftest: PASS %-22s fmt=3 stride=%u scale=%d\n",
           "linear-no-region-bleed", stride, s_scale);
    return 1;
}

/* Distinct GE minification/magnification filters must survive the production sampler path.
 * The two cases deliberately render at 2x magnification and 2x minification; the selected
 * filter is NEAREST in each case, so an alternating texture must contain no blended colors. */
#define COH_TFLT_RECT_X 120
#define COH_TFLT_RECT_Y 120

static uint32_t coherence_tflt_texel(unsigned u, unsigned v) {
    (void)v;
    return (u & 1u) ? 0xFFFFFFFFu : 0xFF000000u;
}

static uint32_t coherence_prepare_tflt_fixture(uint32_t tex_base, uint32_t tflt, int primitive_type,
                                               int rect_w, int rect_h,
                                               float u0, float v0, float u_span, float v_span) {
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);
    for (unsigned v = 0; v < 8u; v++)
        for (unsigned u = 0; u < 8u; u++)
            tex[v * 8u + u] = coherence_tflt_texel(u, v);
    sr_gpu_vram_dirty(tex_base, 8u * 8u * 4u);

    const uint32_t vaddr = 0x00101000u;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    vtx[0] = (RawSpriteVtx){ u0, v0, (float)COH_TFLT_RECT_X,
                             (float)COH_TFLT_RECT_Y, 0.0f };
    vtx[1] = (RawSpriteVtx){ u0 + u_span, v0 + v_span,
                             (float)(COH_TFLT_RECT_X + rect_w),
                             (float)(COH_TFLT_RECT_Y + rect_h), 0.0f };
    if (primitive_type == 3) {
        vtx[2] = (RawSpriteVtx){ u0, v0 + v_span,
                                 (float)COH_TFLT_RECT_X,
                                 (float)(COH_TFLT_RECT_Y + rect_h), 0.0f };
    }

    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 8u | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (3u << 8) | 3u);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, tflt);
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3u | (1u << 8));
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, ((uint32_t)primitive_type << 16) | (primitive_type == 3 ? 3u : 2u));
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return list_addr;
}

static int coherence_verify_tflt(const char *path, const uint32_t *fb, uint32_t stride,
                                 int rect_w, int rect_h, int expect_linear) {
    int found_blended = 0;
    for (int y = 0; y < rect_h; y++) {
        for (int x = 0; x < rect_w; x++) {
            uint32_t rgb = fb[(COH_TFLT_RECT_Y + y) * stride + COH_TFLT_RECT_X + x] & 0x00FFFFFFu;
            if (rgb != 0x000000u && rgb != 0xFFFFFFu) found_blended = 1;
            if (!expect_linear && rgb != 0x000000u && rgb != 0xFFFFFFu) {
                fprintf(stderr,
                        "gpu coherence selftest [TFLT-%s]: pixel=(%d,%d) unexpectedly blended: %06x\n",
                        path, COH_TFLT_RECT_X + x, COH_TFLT_RECT_Y + y, rgb);
                return 0;
            }
        }
    }
    if (expect_linear && !found_blended) {
        fprintf(stderr, "gpu coherence selftest [TFLT-%s]: expected linear filtering, found only texels\n",
                path);
        return 0;
    }
    return 1;
}

static int coherence_run_tflt_case(const char *name, int primitive_type, uint32_t tflt,
                                   int rect_w, int rect_h,
                                   float u0, float v0, float u_span, float v_span,
                                   int expect_linear) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    uint32_t list_addr = coherence_prepare_tflt_fixture(tex_base, tflt, primitive_type,
                                                        rect_w, rect_h,
                                                        u0, v0, u_span, v_span);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [TFLT-%s]: GPU readback failed\n", name);
        coherence_reset_targets();
        return 0;
    }
    uint32_t *fb = (uint32_t *)guest;
    if (!coherence_verify_tflt("gpu", fb, stride, rect_w, rect_h, expect_linear)) {
        coherence_reset_targets();
        return 0;
    }
    size_t bytes = stride * FB_H * sizeof(*fb);
    uint8_t *reference = (uint8_t *)malloc(bytes);
    if (!reference) {
        fprintf(stderr, "gpu coherence selftest [TFLT-%s]: no reference buffer\n", name);
        coherence_reset_targets();
        return 0;
    }
    memcpy(reference, guest, bytes);
    memset(guest, 0, stride * FB_H * 4u);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    ge_set_gpu_hooks(&k_hooks);
    if (!coherence_verify_tflt("software", fb, stride, rect_w, rect_h, expect_linear)) {
        free(reference);
        coherence_reset_targets();
        return 0;
    }
    size_t pixel_count = bytes / sizeof(*fb);
    size_t mismatch = pixel_count;
    const uint32_t *gpu = (const uint32_t *)reference;
    for (size_t pixel = 0; pixel < pixel_count; pixel++) {
        int differs = 0;
        if (!expect_linear) {
            differs = gpu[pixel] != fb[pixel];
        } else {
            /* Vulkan filtering and the integer software lerp can round by one U8 step. */
            for (unsigned shift = 0; shift < 32; shift += 8) {
                int gpu_channel = (int)((gpu[pixel] >> shift) & 0xFFu);
                int sw_channel = (int)((fb[pixel] >> shift) & 0xFFu);
                int delta = gpu_channel - sw_channel;
                if (delta < -1 || delta > 1) differs = 1;
            }
        }
        if (differs) { mismatch = pixel; break; }
    }
    if (mismatch != pixel_count) {
        fprintf(stderr,
                "gpu coherence selftest [TFLT-%s]: GPU/software mismatch at byte=%zu pixel=(%zu,%zu) gpu=%08x software=%08x\n",
                name, mismatch * sizeof(*fb), mismatch % stride, mismatch / stride,
                gpu[mismatch], fb[mismatch]);
        free(reference);
        coherence_reset_targets();
        return 0;
    }
    free(reference);
    coherence_reset_targets();
    printf("gpu coherence selftest: PASS TFLT %-22s mode=0x%03x\n", name, tflt);
    return 1;
}

static int coherence_run_tflt_software_line(const char *name, uint32_t tflt,
                                            int rect_w, int rect_h,
                                            float u0, float u_span, int expect_linear) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    uint32_t list_addr = coherence_prepare_tflt_fixture(tex_base, tflt, 1,
                                                        rect_w, rect_h,
                                                        u0, 0.0f, u_span, 0.0f);
    ge_set_gpu_hooks(NULL);
    ge_run_list(list_addr, 0);
    int ok = coherence_verify_tflt("software-line", (const uint32_t *)guest, stride,
                                   rect_w, rect_h, expect_linear);
    ge_set_gpu_hooks(&k_hooks);
    coherence_reset_targets();
    if (!ok) return 0;
    printf("gpu coherence selftest: PASS TFLT %-22s mode=0x%03x\n", name, tflt);
    return 1;
}

static int coherence_run_tflt_filters(void) {
    int ok = 1;
    /* min=linear, mag=nearest; this sprite is magnified, so nearest must win. */
    if (!coherence_run_tflt_case("mag-nearest", 6, 0x001u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 0))
        ok = 0;
    /* min=nearest, mag=linear; this sprite is minified, so nearest must win. */
    if (!coherence_run_tflt_case("min-nearest", 6, 0x100u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 0))
        ok = 0;
    /* The complementary cases prove linear filtering still wins when its field is selected. */
    if (!coherence_run_tflt_case("mag-linear", 6, 0x100u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 1))
        ok = 0;
    if (!coherence_run_tflt_case("min-linear", 6, 0x001u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 1))
        ok = 0;
    /* The triangle path uses the per-pixel texture planes rather than sprite steps. */
    if (!coherence_run_tflt_case("triangle-mag-nearest", 3, 0x001u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_case("triangle-min-nearest", 3, 0x100u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_case("triangle-mag-linear", 3, 0x100u, 16, 16,
                                 0.0f, 0.0f, 8.0f, 8.0f, 1))
        ok = 0;
    if (!coherence_run_tflt_case("triangle-min-linear", 3, 0x001u, 4, 4,
                                 0.25f, 0.25f, 8.0f, 8.0f, 1))
        ok = 0;
    /* Lines are software-rasterized for this fixture; exercise their DDA footprint too. */
    if (!coherence_run_tflt_software_line("line-mag-nearest", 0x001u, 16, 2,
                                          0.0f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_software_line("line-min-nearest", 0x100u, 4, 2,
                                          0.25f, 8.0f, 0))
        ok = 0;
    if (!coherence_run_tflt_software_line("line-mag-linear", 0x100u, 16, 2,
                                          0.0f, 8.0f, 1))
        ok = 0;
    if (!coherence_run_tflt_software_line("line-min-linear", 0x001u, 4, 2,
                                          0.25f, 8.0f, 1))
        ok = 0;
    return ok;
}

/* ---- texture mip levels (#1373) ----------------------------------------------------------
 *
 * The Vulkan backend samples TEXMODE/TEXLEVEL mip levels from one Vulkan mip chain per cached
 * texture and must choose what ge.c tex_detail16 and ge_tex_level_pick choose. Each case draws one through-mode sprite
 * with the Vulkan backend and then with the software GE, and compares the framebuffers:
 * exactly for nearest-level cases, and within the one-U8-step tolerance of the TFLT linear
 * cases for a blend of two levels. Every texel names its level and position, so a wrong
 * level, a level decoded without its own buffer width, or a level coordinate one texel off
 * changes a pixel. The UV start of 0.25 texel keeps every sample off a texel edge at every
 * level, and the footprints (1/2, 2 and 4 texels per pixel) give exact levels of detail.
 * Software/Vulkan agreement is not a PSP measurement; the level rules both paths share are
 * PSP-GE-MIP-001's (hle_thread_selftest.c test_ge_mip_cells_match_psp checks ge.c against
 * the PSP). The automatic and slope cases draw on the GPU, which picks their level per
 * fragment in psp.frag (F_LODFRAG); each case also checks that only a level list a halving
 * chain cannot hold takes the software rasterizer. */
#define COH_MIP_TEX   0x04040000u          /* level L at COH_MIP_TEX + L * 0x1000 */
#define COH_MIP_ALT1  0x04045000u          /* a second level 1 (4x4) for the cache-key case */
#define COH_MIP_LIST  0x00100800u
#define COH_MIP_RESET 0x00100C00u
#define COH_MIP_VERTS 0x00101800u
#define COH_MIP_X     120
#define COH_MIP_Y     120

static uint32_t coherence_mip_texel(unsigned level, unsigned u, unsigned v) {
    uint32_t r = level * 64u + u * 8u + v, g = 255u - r, b = 40u * level + 30u;
    return 0xFF000000u | (b << 16) | (g << 8) | r;
}

/* The second level 1 differs from the first in every colour bit. */
static uint32_t coherence_mip_alt_texel(unsigned u, unsigned v) {
    return coherence_mip_texel(1u, u, v) ^ 0x00FFFFFFu;
}

/* Levels 0-3 of an 8x8 8888 texture. Level 1 keeps its 4 texels per row in an 8-wide buffer
 * with magenta beyond them, which only a decode that ignores the level's own buffer width
 * reaches; the second level 1 is a plain 4x4. Levels 2 and 3 use buffer width 4: a linear
 * texture's rows are whole 16-byte lines (ge_tex_row_bytes). */
static unsigned coherence_mip_bufw(unsigned level) {
    return level <= 1u ? 8u : 4u;
}

static void coherence_mip_fill(void) {
    for (unsigned level = 0; level < 4u; level++) {
        unsigned size = 8u >> level, bufw = coherence_mip_bufw(level);
        uint32_t addr = COH_MIP_TEX + level * 0x1000u;
        uint32_t *tex = (uint32_t *)SR_HOST(addr);
        for (unsigned v = 0; v < size; v++)
            for (unsigned u = 0; u < bufw; u++)
                tex[v * bufw + u] = u < size ? coherence_mip_texel(level, u, v) : 0xFFFF00FFu;
        sr_gpu_vram_dirty(addr, size * bufw * 4u);
    }
    uint32_t *alt = (uint32_t *)SR_HOST(COH_MIP_ALT1);
    for (unsigned v = 0; v < 4u; v++)
        for (unsigned u = 0; u < 4u; u++) alt[v * 4u + u] = coherence_mip_alt_texel(u, v);
    sr_gpu_vram_dirty(COH_MIP_ALT1, 4u * 4u * 4u);
}

typedef struct CohMipCase {
    const char *name;
    uint32_t tflt;              /* TEXFILTER (magnification field in bits 8+) */
    uint32_t max_level;         /* TEXMODE highest level */
    uint32_t mode, bias16;      /* TEXLEVEL mode, and its bias in 1/16 (8-bit two's complement) */
    int rect;                   /* sprite side in pixels; the UV span is 8 texels */
    int tolerance;              /* per-channel U8 tolerance: 1 for a blend of two levels */
    int level;                  /* level the first pixel samples, or -1 for a blend */
    int alt_level1;             /* level 1 is the second (plain 4x4) level 1 */
    uint32_t level1_log2;       /* level 1 TEXSIZE log2 on both axes; 0 = 2, the halving chain */
    float slope;                /* TEXLODSLOPE */
    int falls_back;             /* the draw takes the software rasterizer */
} CohMipCase;

/* The GE list of one case: framebuffer 0x04160000, levels 0-3, then one sprite when `draw`
 * is set (otherwise the state alone, for direct hook draws). */
static uint32_t coherence_mip_list(const CohMipCase *c, int draw) {
    if (draw) {
        RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(COH_MIP_VERTS);
        vtx[0] = (RawSpriteVtx){ 0.25f, 0.25f, (float)COH_MIP_X, (float)COH_MIP_Y, 0.0f };
        vtx[1] = (RawSpriteVtx){ 8.25f, 8.25f, (float)(COH_MIP_X + c->rect),
                                 (float)(COH_MIP_Y + c->rect), 0.0f };
    }
    uint32_t *dl = (uint32_t *)SR_HOST(COH_MIP_LIST);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0x9E, 0x00080000u);                         /* depth buffer clear of the levels */
    DL_EMIT(0x9F, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    for (uint32_t level = 0; level < 4u; level++) {
        uint32_t addr = COH_MIP_TEX + level * 0x1000u, bufw = coherence_mip_bufw(level);
        uint32_t log2 = 3u - level;
        if (level == 1u) {
            addr = c->alt_level1 ? COH_MIP_ALT1 : addr;
            bufw = c->alt_level1 ? 4u : 8u;
            log2 = c->level1_log2 ? c->level1_log2 : 2u;
        }
        DL_EMIT(0xA0 + level, addr & 0x00FFFFFFu);                        /* TEXADDRn */
        DL_EMIT(0xA8 + level, bufw | ((addr & 0xFF000000u) >> 8));         /* TEXBUFWIDTHn */
        DL_EMIT(0xB8 + level, (log2 << 8) | log2);                         /* TEXSIZEn */
    }
    DL_EMIT(0xC2, c->max_level << 16);                  /* TEXMODE: highest level, unswizzled */
    DL_EMIT(0xC8, c->mode | ((c->bias16 & 0xFFu) << 16));                  /* TEXLEVEL */
    { uint32_t slope_bits; memcpy(&slope_bits, &c->slope, 4);
      DL_EMIT(0xD0, slope_bits >> 8); }                                     /* TEXLODSLOPE */
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC6, c->tflt);
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3u | (1u << 8));                      /* replace, RGBA */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));  /* float UV and position, through */
    if (draw) {
        DL_EMIT(0x01, COH_MIP_VERTS & 0x00FFFFFFu);
        DL_EMIT(0x04, (6u << 16) | 2u);                 /* one sprite */
    }
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    return COH_MIP_LIST;
}

/* Texture sample points measured on a PSP-3000 (PSP-GE-SAMPLE-001, issue #282). A 32x32 8888
 * texture shows texel (u, v) as R = 8u, G = 8v, so a pixel's R and G name its texel and, under
 * bilinear filtering, its blend. Four through-mode cells from the record, each 8x8 pixels:
 *   A  triangle pair, bilinear, 1:1 at integer UV 8..16: every pixel is exactly its texel;
 *   B  the same with nearest filtering;
 *   C  triangle pair, nearest, UV 8.5..16.5: texel 9 + dx, 9 + dy;
 *   D  sprite, bilinear, 2x magnified (UV 8..12): the first pixel blends texel 7 at 4/16
 *      and texel 8 at 12/16 on each axis, the second texel 8 at 12/16 and texel 9 at 4/16.
 * Before the rule was measured, a 1:1 triangle blended half of the next texel under bilinear
 * filtering (a glyph's last row picked up the next glyph's first) and showed the next texel
 * under nearest. Both the Vulkan path and the software rasterizer are checked. */
#define COH_SAMPLE_TEX   0x04048000u
#define COH_SAMPLE_LIST  0x00102000u
#define COH_SAMPLE_VERTS 0x00102800u
static int coherence_sample_expect(int cell, int dx, int dy, uint32_t *want) {
    int r, g;
    if (cell == 0 || cell == 1) { r = 8 * (8 + dx); g = 8 * (8 + dy); }
    else if (cell == 2) { r = 8 * (9 + dx); g = 8 * (9 + dy); }
    else {
        static const int first_two[2] = { (56 * 4 + 64 * 12) / 16, (64 * 12 + 72 * 4) / 16 };
        if (dx > 1 || dy > 1) return 0;
        r = first_two[dx]; g = first_two[dy];
    }
    *want = 0xFF000000u | ((uint32_t)g << 8) | (uint32_t)r;
    return 1;
}

static int coherence_run_psp_sample_points(void) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    const size_t bytes = (size_t)stride * FB_H * 4u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(COH_SAMPLE_TEX);
    for (unsigned v = 0; v < 32u; v++)
        for (unsigned u = 0; u < 32u; u++) tex[v * 32u + u] = 0xFF000000u | ((8u * v) << 8) | (8u * u);
    sr_gpu_vram_dirty(COH_SAMPLE_TEX, 32u * 32u * 4u);

    static const struct { int x, type; float u0, u1; uint32_t tflt; } cells[4] = {
        { 100, 3, 8.0f, 16.0f, 0x101u }, { 110, 3, 8.0f, 16.0f, 0x000u },
        { 120, 3, 8.5f, 16.5f, 0x000u }, { 130, 6, 8.0f, 12.0f, 0x101u },
    };
    const int y0 = 10;
    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(COH_SAMPLE_VERTS);
    uint32_t *dl = (uint32_t *)SR_HOST(COH_SAMPLE_LIST);
    int p = 0, nv = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, 0x00160000u);
    DL_EMIT(0x9D, 512u | (0x04000000u >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x4C, 0);
    DL_EMIT(0x4D, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0x15, 0);
    DL_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, COH_SAMPLE_TEX & 0x00FFFFFFu);
    DL_EMIT(0xA8, 32u | ((COH_SAMPLE_TEX & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (5u << 8) | 5u);
    DL_EMIT(0xC2, 0);
    DL_EMIT(0xC8, 0);
    DL_EMIT(0xC0, 0);
    DL_EMIT(0xC3, 3);
    DL_EMIT(0xC7, (1u << 8) | 1u);                         /* clamp u and v */
    DL_EMIT(0xC9, 3u | (1u << 8));                         /* replace, RGBA */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));     /* float UV and position, through */
    for (int c = 0; c < 4; c++) {
        float x0 = (float)cells[c].x, x1 = x0 + 8.0f, ya = (float)y0, yb = ya + 8.0f;
        float u0 = cells[c].u0, u1 = cells[c].u1;
        uint32_t first = COH_SAMPLE_VERTS + (uint32_t)nv * (uint32_t)sizeof(RawSpriteVtx);
        DL_EMIT(0xC6, cells[c].tflt);
        DL_EMIT(0xCB, 0);                                  /* TEXFLUSH */
        if (cells[c].type == 3) {                          /* TL TR BL, BL TR BR */
            vtx[nv++] = (RawSpriteVtx){ u0, u0, x0, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u0, x1, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u0, u1, x0, yb, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u0, u1, x0, yb, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u0, x1, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u1, x1, yb, 0.0f };
            DL_EMIT(0x01, first & 0x00FFFFFFu);
            DL_EMIT(0x04, (3u << 16) | 6u);
        } else {
            vtx[nv++] = (RawSpriteVtx){ u0, u0, x0, ya, 0.0f };
            vtx[nv++] = (RawSpriteVtx){ u1, u1, x1, yb, 0.0f };
            DL_EMIT(0x01, first & 0x00FFFFFFu);
            DL_EMIT(0x04, (6u << 16) | 2u);
        }
    }
    DL_EMIT(0xC6, 0);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT

    uint32_t *gpu = (uint32_t *)malloc(bytes);
    if (!gpu) return 0;
    coherence_reset_targets();
    memset(fb, 0, bytes);
    sr_gpu_vram_dirty(base, (uint32_t)bytes);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(COH_SAMPLE_LIST, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    int ok = gegpu_sync_guest_fb(&desc);
    if (!ok) fprintf(stderr, "gpu coherence selftest [psp-sample-points]: GPU readback failed\n");
    if (ok) {
        memcpy(gpu, fb, bytes);
        memset(fb, 0, bytes);
        ge_set_gpu_hooks(NULL);
        ge_run_list(COH_SAMPLE_LIST, 0);
        ge_set_gpu_hooks(&k_hooks);
    }
    static const char *const names[4] = { "tri-linear-1to1", "tri-nearest-1to1",
                                          "tri-nearest-half", "spr-linear-mag2" };
    for (int c = 0; ok && c < 4; c++)
        for (int dy = 0; ok && dy < 8; dy++)
            for (int dx = 0; ok && dx < 8; dx++) {
                uint32_t want;
                if (!coherence_sample_expect(c, dx, dy, &want)) continue;
                size_t at = (size_t)(y0 + dy) * stride + (size_t)(cells[c].x + dx);
                uint32_t sw = fb[at] & 0x00FFFFFFu, gp = gpu[at] & 0x00FFFFFFu;
                want &= 0x00FFFFFFu;
                if (sw != want || gp != want) {
                    fprintf(stderr, "gpu coherence selftest [psp-sample-points/%s]: pixel=(%d,%d) "
                            "software=%06x gpu=%06x, want %06x (PSP-GE-SAMPLE-001)\n",
                            names[c], dx, dy, sw, gp, want);
                    ok = 0;
                }
            }
    free(gpu);
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS %-22s 4 cells (PSP-GE-SAMPLE-001)\n", "psp-sample-points");
    return ok;
}

/* Put TEXMODE, TEXLEVEL and TEXFILTER back to level 0 only through the list path, so the
 * later fixtures start from the state they assume and the state serial moves with it. */
static void coherence_mip_reset_state(void) {
    uint32_t *dl = (uint32_t *)SR_HOST(COH_MIP_RESET);
    dl[0] = 0xC2u << 24;
    dl[1] = 0xC8u << 24;
    dl[2] = 0xC6u << 24;
    dl[3] = 0x0Fu << 24;
    dl[4] = 0x0Cu << 24;
    ge_run_list(COH_MIP_RESET, 0);
}

/* The texel the first pixel shows: the sprite's UV runs 0.25..8.25 level-0 texels across
 * `rect` pixels, and the pixel samples it at its centre (PSP-GE-SAMPLE-001), so it reads
 * s = 0.25 + 0.5 * 8 / rect, which is s / 2^level in the level. Every case with an expected
 * level filters that level by the nearest rule, floor(s). */
static uint32_t coherence_mip_first_texel(const CohMipCase *c) {
    float s = (0.25f + 4.0f / (float)c->rect) / (float)(1u << (unsigned)c->level);
    unsigned size = 8u >> (unsigned)c->level, t = (unsigned)s;
    if (t >= size) t = size - 1u;
    return c->level == 1 && c->alt_level1 ? coherence_mip_alt_texel(t, t)
                                          : coherence_mip_texel((unsigned)c->level, t, t);
}

static int coherence_run_mip_case(const CohMipCase *c) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    const size_t bytes = (size_t)stride * FB_H * 4u, pixels = bytes / 4u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *gpu = (uint32_t *)malloc(bytes);
    if (!gpu) {
        fprintf(stderr, "gpu coherence selftest [mip-%s]: no reference buffer\n", c->name);
        return 0;
    }
    uint32_t list = coherence_mip_list(c, 1);
    coherence_reset_targets();
    memset(fb, 0, bytes);
    sr_gpu_vram_dirty(base, (uint32_t)bytes);
    ge_set_gpu_hooks(&k_hooks);
    const unsigned long long fallback0 = s_replay_stats.fallback_prims;
    ge_run_list(list, 0);
    const unsigned long long fallbacks = s_replay_stats.fallback_prims - fallback0;
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    int ok = gegpu_sync_guest_fb(&desc);
    if (!ok) fprintf(stderr, "gpu coherence selftest [mip-%s]: GPU readback failed\n", c->name);
    if (ok && (fallbacks != 0u) != (c->falls_back != 0)) {
        fprintf(stderr, "gpu coherence selftest [mip-%s]: %llu software fallback primitive(s), want %s\n",
                c->name, fallbacks, c->falls_back ? "the software rasterizer" : "the GPU");
        ok = 0;
    }
    if (ok) {
        memcpy(gpu, fb, bytes);
        memset(fb, 0, bytes);
        ge_set_gpu_hooks(NULL);
        ge_run_list(list, 0);
        ge_set_gpu_hooks(&k_hooks);
    }
    uint32_t first = (uint32_t)(COH_MIP_Y * (int)stride + COH_MIP_X);
    if (ok && c->level >= 0) {
        uint32_t want = coherence_mip_first_texel(c) & 0x00FFFFFFu;
        if ((fb[first] & 0x00FFFFFFu) != want || (gpu[first] & 0x00FFFFFFu) != want) {
            fprintf(stderr,
                    "gpu coherence selftest [mip-%s]: first pixel gpu=%06x software=%06x, want "
                    "level %d texel %06x\n", c->name, gpu[first] & 0x00FFFFFFu,
                    fb[first] & 0x00FFFFFFu, c->level, want);
            ok = 0;
        }
    }
    for (size_t px = 0; ok && px < pixels; px++) {
        int differs = 0;
        for (unsigned shift = 0; shift < 32u; shift += 8u) {
            int delta = (int)((gpu[px] >> shift) & 0xFFu) - (int)((fb[px] >> shift) & 0xFFu);
            if (delta < -c->tolerance || delta > c->tolerance) differs = 1;
        }
        if (differs) {
            fprintf(stderr,
                    "gpu coherence selftest [mip-%s]: GPU/software mismatch at pixel=(%zu,%zu) "
                    "gpu=%08x software=%08x\n",
                    c->name, px % stride, px / stride, gpu[px], fb[px]);
            ok = 0;
        }
    }
    free(gpu);
    coherence_reset_targets();
    if (ok)
        printf("gpu coherence selftest: PASS mip %-22s mode=%u bias=%d/16 max=%u tflt=0x%03x\n",
               c->name, c->mode, (int)(int8_t)(uint8_t)c->bias16, c->max_level, c->tflt);
    return ok;
}

/* A notified CPU write into mip level 1 between two same-state draws must reach the second
 * draw: hook_vram_dirty has to see the level's own source range, not only level 0's, or the
 * state template keeps the old chain (the texture-dirty-same-state rule, for a level). */
static int coherence_run_mip_level_dirty_same_state(void) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u, rgb = 0x00FFFFFFu;
    const uint32_t level1 = COH_MIP_TEX + 0x1000u;
    const CohMipCase c = { "level-dirty", 0x004u, 3u, 1u, 16u, 8, 0, 1, 0, 0, 0.0f, 0 };
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(level1);
    coherence_reset_targets();
    memset(fb, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(coherence_mip_list(&c, 0), 0);      /* the state, no draw */
    GeVtx p0 = { (float)COH_MIP_X, (float)COH_MIP_Y, 1.0f, 1.0f, 0.25f, 0.25f, 1.0f,
                 255, 255, 255, 255 };
    GeVtx p1 = { (float)(COH_MIP_X + 8), (float)(COH_MIP_Y + 8), 1.0f, 1.0f, 8.25f, 8.25f, 1.0f,
                 255, 255, 255, 255 };
    int ok = hook_sprite(&p0, &p1, 0);
    for (unsigned v = 0; v < 4u; v++)
        for (unsigned u = 0; u < 4u; u++) tex[v * 8u + u] = coherence_mip_alt_texel(u, v);
    sr_gpu_vram_dirty(level1, 4u * 8u * 4u);
    p0.x += 16.0f; p1.x += 16.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    uint32_t left = fb[COH_MIP_Y * stride + COH_MIP_X] & rgb;
    uint32_t right = fb[COH_MIP_Y * stride + COH_MIP_X + 16] & rgb;
    uint32_t want_left = coherence_mip_texel(1u, 0u, 0u) & rgb;
    uint32_t want_right = coherence_mip_alt_texel(0u, 0u) & rgb;
    if (!ok || left != want_left || right != want_right) {
        fprintf(stderr,
                "gpu coherence selftest [mip-level-dirty]: draws=%d left=%06x (want %06x) "
                "right=%06x (want %06x)\n", ok, left, want_left, right, want_right);
        ok = 0;
    }
    coherence_mip_fill();                           /* level 1 back to its first contents */
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS mip %-22s old=%06x new=%06x\n",
                   "level-dirty-same-state", left, right);
    return ok;
}

/* One GPU draw through the hooks, after the state of `c`: a size x size quad at (COH_MIP_X,
 * COH_MIP_Y) whose texel coordinates run from 0.25 by `span` texels across it (`vspan` down
 * it, or `span` when 0), at 1/w = `rw`
 * everywhere (texel coordinates pre-multiplied by it, as hook_tri receives them), or a
 * through-mode sprite when `persp` is 0. Returns the level the pixel one in from the corner
 * sampled (red >> 6 of coherence_mip_texel), or -1. */
static int coherence_mip_hook_level(const CohMipCase *c, int persp, float rw, int size, float span,
                                    float vspan) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u;
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    coherence_reset_targets();
    memset(fb, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(coherence_mip_list(c, 0), 0);      /* the state, no draw */
    s_ge->minz = 0u;
    s_ge->maxz = 65535u;
    const float x0 = (float)COH_MIP_X, y0 = (float)COH_MIP_Y, x1 = x0 + (float)size, y1 = y0 + (float)size;
    const float u0 = 0.25f, u1 = 0.25f + span, v1 = 0.25f + (vspan > 0.0f ? vspan : span);
    const float q = persp ? rw : 1.0f;
    GeVtx a = { x0, y0, 1.0f, q, u0 * q, u0 * q, 1.0f, 255, 255, 255, 255 };
    GeVtx b = { x1, y0, 1.0f, q, u1 * q, u0 * q, 1.0f, 255, 255, 255, 255 };
    GeVtx d = { x0, y1, 1.0f, q, u0 * q, v1 * q, 1.0f, 255, 255, 255, 255 };
    GeVtx e = { x1, y1, 1.0f, q, u1 * q, v1 * q, 1.0f, 255, 255, 255, 255 };
    const unsigned long long fallback0 = s_replay_stats.fallback_prims;
    int ok = persp ? hook_tri(&a, &b, &e, 1) && hook_tri(&a, &e, &d, 1) : hook_sprite(&a, &e, 0);
    ok = ok && s_replay_stats.fallback_prims == fallback0 && s_cur && target_readback(s_cur);
    if (!ok) return -1;
    const uint32_t px = fb[(COH_MIP_Y + 1) * stride + COH_MIP_X + 1];
    const uint32_t r = px & 0xFFu, g = (px >> 8) & 0xFFu;
    return g == 255u - r ? (int)(r >> 6) : -1;
}

/* Per-fragment levels of detail through the hooks (#1373): psp.frag takes q from the
 * interpolated 1/w in slope mode, and in automatic mode the footprint from the quotient
 * rule raster_tri uses, (d(u/w) - u d(1/w)) / (1/w), per PSP pixel. Only the level is
 * checked, so this also runs alone at SR_GPU_SCALE=2 and 4 (SR_GPU_COHERENCE_CASE=mip-lod),
 * where a device pixel's footprint is 1/scale of a PSP pixel's. */
static int coherence_run_mip_lod(void) {
    static const struct {
        const char *name;
        CohMipCase c;
        int persp; float rw; int size; float span; int level; float vspan;
    } cells[] = {
        /* w = 2, slope 1: 16 + log16(1) - log16(1/2) = 32, level 2, whatever the footprint. */
        { "persp-slope", { "persp-slope", 0x004u, 3u, 2u, 0u, 0, 0, 0, 0, 0, 1.0f, 0 }, 1, 0.5f, 16, 8.0f, 2, 0.0f },
        /* w = 2, 16 texels over 4 pixels: footprint 4, level 2. Leaving d(u/w) undivided by
         * 1/w reads footprint 2, level 1. */
        { "persp-auto", { "persp-auto", 0x004u, 3u, 0u, 0u, 0, 0, 0, 0, 0, 0.0f, 0 }, 1, 0.5f, 4, 16.0f, 2, 0.0f },
        { "persp-auto-mag", { "persp-auto-mag", 0x004u, 3u, 0u, 0u, 0, 0, 0, 0, 0, 0.0f, 0 }, 1, 0.5f, 16, 8.0f, 0, 0.0f },
        /* Through mode, 8 texels over 4 pixels: footprint 2, level 1. */
        { "through-auto", { "through-auto", 0x004u, 3u, 0u, 0u, 0, 0, 0, 0, 0, 0.0f, 0 }, 0, 1.0f, 4, 8.0f, 1, 0.0f },
        /* The larger axis: 2 texels across and 8 down 4 pixels is footprint 2, level 1. */
        { "through-auto-v", { "through-auto-v", 0x004u, 3u, 0u, 0u, 0, 0, 0, 0, 0, 0.0f, 0 }, 0, 1.0f, 4, 2.0f, 1,
          8.0f },
    };
    int ok = 1;
    coherence_mip_fill();
    for (size_t i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        const int level = coherence_mip_hook_level(&cells[i].c, cells[i].persp, cells[i].rw,
                                                   cells[i].size, cells[i].span, cells[i].vspan);
        if (level != cells[i].level) {
            fprintf(stderr, "gpu coherence selftest [mip-lod %s]: level %d at scale %d, want %d\n",
                    cells[i].name, level, s_scale, cells[i].level);
            ok = 0;
        } else {
            printf("gpu coherence selftest: PASS mip-lod %-18s level=%d scale=%d\n", cells[i].name,
                   level, s_scale);
        }
    }
    coherence_mip_reset_state();
    coherence_reset_targets();
    return ok;
}

/* A TEXLODSLOPE write between two draws of an otherwise identical state must reach the second
 * draw: the per-draw state template is keyed by the slope too (StateSnap). Slope 1 is level 1
 * and slope 2 level 2 in through mode. */
static int coherence_run_mip_slope_change(void) {
    const uint32_t base = 0x04000000u | 0x00160000u, stride = 512u, rgb = 0x00FFFFFFu;
    const CohMipCase c = { "slope-change", 0x004u, 3u, 2u, 0u, 16, 0, 1, 0, 0, 1.0f, 0 };
    uint32_t *fb = (uint32_t *)SR_HOST(base);
    coherence_mip_fill();
    coherence_reset_targets();
    memset(fb, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(coherence_mip_list(&c, 0), 0);      /* the state, no draw */
    GeVtx p0 = { (float)COH_MIP_X, (float)COH_MIP_Y, 1.0f, 1.0f, 0.25f, 0.25f, 1.0f,
                 255, 255, 255, 255 };
    GeVtx p1 = { (float)(COH_MIP_X + 16), (float)(COH_MIP_Y + 16), 1.0f, 1.0f, 8.25f, 8.25f, 1.0f,
                 255, 255, 255, 255 };
    int ok = hook_sprite(&p0, &p1, 0);
    uint32_t *dl = (uint32_t *)SR_HOST(COH_MIP_RESET);
    const float two = 2.0f;
    uint32_t bits;
    memcpy(&bits, &two, 4);
    dl[0] = (0xD0u << 24) | (bits >> 8);            /* TEXLODSLOPE 2 */
    dl[1] = 0x0Fu << 24;
    dl[2] = 0x0Cu << 24;
    ge_run_list(COH_MIP_RESET, 0);
    p0.x += 32.0f; p1.x += 32.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    const uint32_t left = fb[COH_MIP_Y * stride + COH_MIP_X] & rgb;
    const uint32_t right = fb[COH_MIP_Y * stride + COH_MIP_X + 32] & rgb;
    const uint32_t want_left = coherence_mip_texel(1u, 0u, 0u) & rgb;
    const uint32_t want_right = coherence_mip_texel(2u, 0u, 0u) & rgb;
    if (!ok || left != want_left || right != want_right) {
        fprintf(stderr,
                "gpu coherence selftest [mip-slope-change]: draws=%d left=%06x (want %06x) "
                "right=%06x (want %06x)\n", ok, left, want_left, right, want_right);
        ok = 0;
    }
    coherence_reset_targets();
    if (ok) printf("gpu coherence selftest: PASS mip %-22s old=%06x new=%06x\n",
                   "slope-change-same-state", left, right);
    return ok;
}

static int coherence_run_mip_levels(void) {
    static const CohMipCase cases[] = {
        /* Constant mode: the bias is the level, clamped to the highest level; one half
         * rounds up to the next level (PSP-GE-MIP-001 c-b0f8). */
        { "const-level-1",       0x004u, 3u, 1u, 16u, 16, 0, 1, 0, 0, 0.0f, 0 },
        { "const-level-2",       0x004u, 3u, 1u, 32u, 16, 0, 2, 0, 0, 0.0f, 0 },
        { "const-clamped",       0x004u, 1u, 1u, 48u, 16, 0, 1, 0, 0, 0.0f, 0 },
        { "const-half-rounds-up", 0x004u, 3u, 1u, 8u, 16, 0, 1, 0, 0, 0.0f, 0 },
        { "const-mode-3",        0x004u, 3u, 3u, 32u, 16, 0, 2, 0, 0, 0.0f, 0 },
        /* With mip levels on, the level of detail picks the filter within a level: a
         * magnified sprite at constant level 1 filters by the minification field
         * (nearest), and a minified one at constant level 0 by the magnification field. */
        { "const-mag-linear",    0x104u, 3u, 1u, 16u, 16, 0, 1, 0, 0, 0.0f, 0 },
        { "const-0-mag-filter",  0x005u, 3u, 1u, 0u,   4, 0, 0, 0, 0, 0.0f, 0 },
        /* GU_NEAREST_MIPMAP_LINEAR at constant level 0.5: half of level 0, half of level 1. */
        { "const-between",       0x006u, 3u, 1u, 8u,  16, 1, -1, 0, 0, 0.0f, 0 },
        /* Automatic mode: log2 of the footprint plus the bias, clamped to [0, highest]. */
        { "auto-magnified",      0x004u, 3u, 0u, 0u,  16, 0, 0, 0, 0, 0.0f, 0 },
        { "auto-level-1",        0x004u, 3u, 0u, 0u,   4, 0, 1, 0, 0, 0.0f, 0 },
        { "auto-level-2",        0x004u, 3u, 0u, 0u,   2, 0, 2, 0, 0, 0.0f, 0 },
        { "auto-biased",         0x004u, 3u, 0u, 0xF0u, 2, 0, 1, 0, 0, 0.0f, 0 },
        { "auto-clamped",        0x004u, 1u, 0u, 0u,   2, 0, 1, 0, 0, 0.0f, 0 },
        /* Bias -1.5 on a 2x minified sprite: level of detail -0.5, so level 0 through the
         * magnification field (nearest), not the minification field (linear). */
        { "auto-bias-mag-filter", 0x005u, 3u, 0u, 0xE8u, 4, 0, 0, 0, 0, 0.0f, 0 },
        /* A footprint of 1.6 texels is level of detail 9/16 (the logarithm takes the top
         * four mantissa bits): GU_NEAREST_MIPMAP_LINEAR blends 9/16 of level 1 into level 0. */
        { "auto-between",        0x006u, 3u, 0u, 0u,   5, 1, -1, 0, 0, 0.0f, 0 },
        /* A footprint of 1.33 texels is level of detail 5/16: level 0, but minified, so the
         * minification field (nearest), not the magnification field (linear). */
        { "auto-min-filter",     0x104u, 3u, 0u, 0u,   6, 0, 0, 0, 0, 0.0f, 0 },
        /* Slope mode with TEXLODSLOPE 0: its logarithm is -2032, so level 0. */
        { "slope-zero",          0x004u, 3u, 2u, 16u,  4, 0, 0, 0, 0, 0.0f, 0 },
        /* Slope mode in through mode (q = 1): 16 + log16(slope) + bias, whatever the
         * footprint; 1.5 is 8/16 above 1, which rounds up to level 2. */
        { "slope-level-1",       0x004u, 3u, 2u, 0u,  16, 0, 1, 0, 0, 1.0f, 0 },
        { "slope-level-2",       0x004u, 3u, 2u, 0u,  16, 0, 2, 0, 0, 2.0f, 0 },
        { "slope-half-rounds-up", 0x004u, 3u, 2u, 0u, 16, 0, 2, 0, 0, 1.5f, 0 },
        { "slope-biased",        0x004u, 3u, 2u, 0xF0u, 16, 0, 1, 0, 0, 2.0f, 0 },
        { "slope-clamped",       0x004u, 1u, 2u, 0u,  16, 0, 1, 0, 0, 4.0f, 0 },
        { "slope-between",       0x006u, 3u, 2u, 0u,  16, 1, -1, 0, 0, 1.5f, 0 },
        /* Level 1 declared 8x8 cannot sit in a halving chain: the software rasterizer draws,
         * in constant and per-fragment modes alike. */
        { "chain-fallback",      0x004u, 3u, 1u, 16u, 16, 0, 1, 0, 3u, 0.0f, 1 },
        { "chain-fallback-auto", 0x004u, 3u, 0u, 0u,   4, 0, -1, 0, 3u, 0.0f, 1 },
        /* Only level 1's address changes after const-level-1: the cache must not reuse it. */
        { "level1-moved",        0x004u, 3u, 1u, 16u, 16, 0, 1, 1, 0, 0.0f, 0 },
        { "level1-back",         0x004u, 3u, 1u, 16u, 16, 0, 1, 0, 0, 0.0f, 0 },
    };
    int ok = 1;
    coherence_mip_fill();
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!coherence_run_mip_case(&cases[i])) ok = 0;
    if (!coherence_run_mip_level_dirty_same_state()) ok = 0;
    if (!coherence_run_mip_lod()) ok = 0;
    if (!coherence_run_mip_slope_change()) ok = 0;
    coherence_mip_reset_state();
    coherence_reset_targets();
    return ok;
}

/* ---- ring wrap and blend colour parity (issue #697) -----------------------------------
 *
 * The per-slot vertex arena (VERT_ARENA_VERTS) and per-slot transfer ring are the Vulkan
 * rings a wrap could overrun while the GPU still reads them. A render that does not fit
 * the open recording moves to the next submit slot, and that slot's fence must retire
 * before its arena is written again. These fixtures drive the production hook and GE list
 * paths on synthetic guest memory and check analytic final pixels: an overwrite of
 * in-flight vertex data changes a pixel, and a stale slot cursor changes the last writer.
 *
 * The parity fixtures compare the Vulkan output with the software GE (ge.c, the arbiter)
 * for every blend factor pair, and also report the distance to the project's R6 colour
 * product (ge_raster_ref.c, SPEC_ASSUMPTION until the #343 oracle cells P8/P9 run). */
#include "../ge_raster_ref.h"

static uint32_t wrap_hash(uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; v ^= v >> 16;
    return v;
}

static uint32_t wrap_pixel_rgba(uint32_t pass, uint32_t idx) {
    return 0xff000000u | (wrap_hash(idx * 2654435761u ^ pass * 0x9E3779B9u) & 0xFFFFFFu);
}

static void wrap_vtx(GeVtx *v, float x, float y, uint32_t c) {
    *v = (GeVtx){ x, y, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
                  (int)(c & 0xFFu), (int)((c >> 8) & 0xFFu), (int)((c >> 16) & 0xFFu), (int)(c >> 24) };
}

static void wrap_state(uint32_t base, uint32_t stride) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = 3;
    s_ge->zbp = 0x04080000u;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535;
}

/* Many one-pixel sprites per pass over the whole frame. Each pass rewrites every pixel,
 * so the final frame is the last pass, and every submit slot is reused many times. */
static int coherence_run_ring_wrap(void) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u;
    const uint32_t pixels = (uint32_t)(FB_W * FB_H);
    const uint32_t passes = 3u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    wrap_state(base, stride);
    ge_set_gpu_hooks(&k_hooks);

    GeGpuReplayStats before, after;
    gegpu_replay_stats_get(&before);
    uint64_t spr_before = s_cnt_spr;
    for (uint32_t pass = 0; pass < passes; pass++) {
        for (uint32_t idx = 0; idx < pixels; idx++) {
            GeVtx p0, p1;
            uint32_t c = wrap_pixel_rgba(pass, idx);
            wrap_vtx(&p0, (float)(idx % FB_W), (float)(idx / FB_W), c);
            wrap_vtx(&p1, (float)(idx % FB_W) + 1.0f, (float)(idx / FB_W) + 1.0f, c);
            if (!hook_sprite(&p0, &p1, 0)) {
                fprintf(stderr, "gpu coherence selftest [ring-wrap]: hook_sprite refused pass=%u idx=%u\n",
                        pass, idx);
                coherence_reset_targets();
                return 0;
            }
        }
    }
    Target *t = s_cur;
    if (!t || !target_readback(t)) {
        fprintf(stderr, "gpu coherence selftest [ring-wrap]: target readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_replay_stats_get(&after);

    for (uint32_t idx = 0; idx < pixels; idx++) {
        uint32_t want = wrap_pixel_rgba(passes - 1u, idx);
        /* Non-clear draws write RGB only: alpha is the PSP stencil plane (build_state
         * cmask), so it must keep the initial zero from the cleared frame. */
        if ((guest[idx] >> 24) != 0 || (guest[idx] & 0xFFFFFFu) != (want & 0xFFFFFFu)) {
            fprintf(stderr, "gpu coherence selftest [ring-wrap]: idx=%u (x=%u,y=%u) rgb got=%06x want=%06x "
                            "alpha=%02x\n", idx, idx % FB_W, idx / FB_W, guest[idx] & 0xFFFFFFu,
                    want & 0xFFFFFFu, guest[idx] >> 24);
            coherence_reset_targets();
            return 0;
        }
    }
    uint64_t sprites = s_cnt_spr - spr_before;
    unsigned long long submits = after.render_submits - before.render_submits;
    if (sprites != (uint64_t)pixels * passes) {
        fprintf(stderr, "gpu coherence selftest [ring-wrap]: sprite count %llu\n",
                (unsigned long long)sprites);
        coherence_reset_targets();
        return 0;
    }
    /* The vertex arena overflows once per VERT_ARENA_VERTS, so this fixture forces many
     * arena flushes; more than one full slot cycle proves a recycled, fenced slot. */
    if (submits <= SUBMIT_FRAMES) {
        fprintf(stderr, "gpu coherence selftest [ring-wrap]: only %llu render submits, slot ring did not recycle\n",
                submits);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s sprites=%llu verts=%llu render_submits=%llu\n",
           "ring-wrap", (unsigned long long)sprites,
           (unsigned long long)sprites * 6ull, submits);
    coherence_reset_targets();
    return 1;
}

/* Repeated uploads of different guest content into one target. Each upload is a batched
 * transfer-ring operation; the final GPU image must equal the last content with the one
 * sprite drawn on top, proving the staged copies were not overwritten before the copy ran. */
static int coherence_run_upload_sequence(void) {
    const uint32_t fba = 0x00120000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u;
    const uint32_t bytes = stride * FB_H * 4u;
    const uint32_t rounds = 12u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);

    coherence_reset_targets();
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) guest[i] = wrap_pixel_rgba(0u, i);
    sr_gpu_vram_dirty(base, bytes);
    Target *t = target_color_acquire(fba, stride, 3, 1);
    if (!t || !t->gpu_valid) {
        fprintf(stderr, "gpu coherence selftest [upload-sequence]: could not establish target\n");
        coherence_reset_targets();
        return 0;
    }
    GeGpuReplayStats before, after;
    gegpu_replay_stats_get(&before);
    for (uint32_t k = 1; k <= rounds; k++) {
        for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) guest[i] = wrap_pixel_rgba(k, i);
        sr_gpu_vram_dirty(base, bytes);
        if (!target_upload(t)) {
            fprintf(stderr, "gpu coherence selftest [upload-sequence]: upload %u failed\n", k);
            coherence_reset_targets();
            return 0;
        }
    }
    wrap_state(base, stride);
    ge_set_gpu_hooks(&k_hooks);
    GeVtx p0, p1;
    const uint32_t c = 0xff102030u;
    wrap_vtx(&p0, 0.0f, 0.0f, c);
    wrap_vtx(&p1, 1.0f, 1.0f, c);
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [upload-sequence]: draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_replay_stats_get(&after);
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) {
        uint32_t want = i == 0 ? c : wrap_pixel_rgba(rounds, i);
        if (guest[i] != want) {
            fprintf(stderr, "gpu coherence selftest [upload-sequence]: idx=%u got=%08x want=%08x\n",
                    i, guest[i], want);
            coherence_reset_targets();
            return 0;
        }
    }
    printf("gpu coherence selftest: PASS %-22s uploads=%u reservations=%llu fallbacks=%llu\n",
           "upload-sequence", rounds,
           after.upload_ring_reservations - before.upload_ring_reservations,
           after.upload_ring_fallbacks - before.upload_ring_fallbacks);
    coherence_reset_targets();
    return 1;
}

/* ---- notified texture rewrite between two same-state draws ------------------------------
 *
 * A CPU or DMAC write into a cached texture reaches hook_vram_dirty. Invalidating the
 * texture entry alone is not enough: the cached state template still carries that
 * texture's descriptor, and a following draw whose GE registers are unchanged takes the
 * template fast path in state_get without consulting tex_get, so it samples the old
 * image. This fixture draws one sprite, rewrites the whole texture and notifies the
 * write, draws a second sprite with identical registers, and requires the first sprite
 * to keep the old texels and the second to show the new ones. */
static int coherence_run_texture_dirty_same_state(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t old_texel = 0xFF0000FFu, new_texel = 0xFF00FF00u;   /* ABGR red, green */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    for (unsigned i = 0; i < 64u; i++) tex[i] = old_texel;
    sr_gpu_vram_dirty(tex_base, 64u * 4u);

    wrap_state(base, stride);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_base;
    s_ge->tex_bufw = 8;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 8;
    s_ge->tex_h = 8;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 18.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    uint64_t invalidations_before = s_cnt_tex_invalidations;
    if (!hook_sprite(&p0, &p1, 0)) {
        fprintf(stderr, "gpu coherence selftest [texture-dirty-same-state]: first draw not accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* The notified write under test: same address, same size, new bytes, no GE
     * register change and no CPU-batch boundary before the next draw. */
    for (unsigned i = 0; i < 64u; i++) tex[i] = new_texel;
    sr_gpu_vram_dirty(tex_base, 64u * 4u);

    wrap_vtx(&p0, 30.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 38.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [texture-dirty-same-state]: second draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }

    uint32_t left = guest[14u * stride + 14u] & rgb;
    uint32_t right = guest[14u * stride + 34u] & rgb;
    if (left != (old_texel & rgb) || right != (new_texel & rgb)) {
        fprintf(stderr,
                "gpu coherence selftest [texture-dirty-same-state]: left=%06x (want %06x) "
                "right=%06x (want %06x)%s\n",
                left, old_texel & rgb, right, new_texel & rgb,
                right == (old_texel & rgb)
                    ? " -- the second draw sampled the stale cached texture" : "");
        coherence_reset_targets();
        return 0;
    }
    if (s_cnt_tex_invalidations != invalidations_before + 1u) {
        fprintf(stderr,
                "gpu coherence selftest [texture-dirty-same-state]: expected one texture "
                "invalidation, counted %llu\n",
                (unsigned long long)(s_cnt_tex_invalidations - invalidations_before));
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s invalidations=1 old=%06x new=%06x\n",
           "texture-dirty-same-state", old_texel & rgb, new_texel & rgb);
    coherence_reset_targets();
    return 1;
}

/* The clean-page index of hook_vram_dirty (#282). A notified store into a page no cached texture
 * reads marks the page, so a second store there skips the texture scan; a texture created later
 * on that page must clear the mark, or the store that next changes its texels is skipped and the
 * draw after it samples the stale cached copy. */
static int coherence_run_texture_clean_page_index(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_a = 0x04040000u, tex_b = 0x04060000u;   /* different 4 KiB pages */
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t red = 0xFF0000FFu, blue = 0xFFFF0000u, green = 0xFF00FF00u;   /* ABGR */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint32_t *a = (uint32_t *)SR_HOST(tex_a);
    uint32_t *b = (uint32_t *)SR_HOST(tex_b);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    for (unsigned i = 0; i < 64u; i++) a[i] = red;
    sr_gpu_vram_dirty(tex_a, 64u * 4u);

    wrap_state(base, stride);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_a;
    s_ge->tex_bufw = 8;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 8;
    s_ge->tex_h = 8;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 18.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: first draw not accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* Texture B's page holds no cached texture yet: the first store scans and marks it, the
     * second skips the scan. */
    uint64_t skips_before = s_cnt_tex_clean_skips;
    for (unsigned i = 0; i < 64u; i++) b[i] = blue;
    sr_gpu_vram_dirty(tex_b, 32u * 4u);
    sr_gpu_vram_dirty(tex_b + 32u * 4u, 32u * 4u);
    if (s_cnt_tex_clean_skips != skips_before + 1u) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: expected the second store "
                "to skip the scan, skipped %llu\n",
                (unsigned long long)(s_cnt_tex_clean_skips - skips_before));
        coherence_reset_targets();
        return 0;
    }

    s_ge->tex_addr = tex_b;
    ge_state_note_external_write();   /* a TEXADDR command would bump the state serial */
    wrap_vtx(&p0, 30.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 38.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: second draw not accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* The store under test: B's texels change with no GE register change before the draw. */
    uint64_t invalidations_before = s_cnt_tex_invalidations;
    for (unsigned i = 0; i < 64u; i++) b[i] = green;
    sr_gpu_vram_dirty(tex_b, 64u * 4u);
    /* B is now the only entry on its page and invalid: a second store there skips the scan. */
    uint64_t quiet_before = s_cnt_tex_quiet_skips;
    sr_gpu_vram_dirty(tex_b, 64u * 4u);
    if (s_cnt_tex_quiet_skips != quiet_before + 1u) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: expected a quiet-page skip, "
                "skipped %llu\n", (unsigned long long)(s_cnt_tex_quiet_skips - quiet_before));
        coherence_reset_targets();
        return 0;
    }
    wrap_vtx(&p0, 50.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 58.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: third draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t left = guest[14u * stride + 14u] & rgb;
    uint32_t mid = guest[14u * stride + 34u] & rgb;
    uint32_t right = guest[14u * stride + 54u] & rgb;
    if (left != (red & rgb) || mid != (blue & rgb) || right != (green & rgb) ||
        s_cnt_tex_invalidations != invalidations_before + 1u) {
        fprintf(stderr,
                "gpu coherence selftest [texture-clean-page-index]: left=%06x mid=%06x right=%06x "
                "(want %06x %06x %06x), invalidations=%llu (want 1)%s\n",
                left, mid, right, red & rgb, blue & rgb, green & rgb,
                (unsigned long long)(s_cnt_tex_invalidations - invalidations_before),
                right == (blue & rgb) ? " -- the store after the texture was created was skipped" : "");
        coherence_reset_targets();
        return 0;
    }
    /* The third draw re-uploaded B, so its page is no longer quiet: the next store must
     * invalidate it again, and the draw after it must show the new texels. */
    const uint32_t yellow = 0xFF00FFFFu;
    quiet_before = s_cnt_tex_quiet_skips;
    invalidations_before = s_cnt_tex_invalidations;
    for (unsigned i = 0; i < 64u; i++) b[i] = yellow;
    sr_gpu_vram_dirty(tex_b, 64u * 4u);
    wrap_vtx(&p0, 70.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 78.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    if (!hook_sprite(&p0, &p1, 0) || !s_cur || !target_readback(s_cur)) {
        fprintf(stderr, "gpu coherence selftest [texture-clean-page-index]: fourth draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t last = guest[14u * stride + 74u] & rgb;
    if (last != (yellow & rgb) || s_cnt_tex_quiet_skips != quiet_before ||
        s_cnt_tex_invalidations != invalidations_before + 1u) {
        fprintf(stderr,
                "gpu coherence selftest [texture-clean-page-index]: after re-upload last=%06x (want %06x), "
                "quiet skips=%llu (want 0), invalidations=%llu (want 1)%s\n",
                last, yellow & rgb, (unsigned long long)(s_cnt_tex_quiet_skips - quiet_before),
                (unsigned long long)(s_cnt_tex_invalidations - invalidations_before),
                last == (green & rgb) ? " -- a stale quiet mark skipped the store" : "");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s clean=1 quiet=1 invalidations=2\n", "texture-clean-page-index");
    coherence_reset_targets();
    return 1;
}

/* A texture in main RAM changes with no notification (guest CPU stores to main RAM are not
 * tracked), so the content hash is what finds it: one texel in the upper half of a hashed
 * 8-byte word turns from red to green between two submissions, and the second draw shows
 * it. */
static int coherence_run_texture_ram_unnotified_change(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x08530000u;
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t red = 0xFF0000FFu, green = 0xFF00FF00u;   /* ABGR */
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint32_t *tex = (uint32_t *)SR_HOST(tex_base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    for (unsigned i = 0; i < 64u; i++) tex[i] = red;

    wrap_state(base, stride);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_base;
    s_ge->tex_bufw = 8;
    s_ge->tex_fmt = 3;
    s_ge->tex_w = 8;
    s_ge->tex_h = 8;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 18.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    int ok = hook_sprite(&p0, &p1, 0);
    /* A submission (a frame's present, say) separates the store from the next draw, as in a
     * title; within one submission an unchanged draw state reuses its template, texture
     * included. */
    ok = ok && submit_pending();
    tex[1] = green;          /* texel (1, 0): bytes 4-7 of the first 8-byte word; no notification */
    wrap_vtx(&p0, 30.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 38.0f, 18.0f, white); p1.u = 8.0f; p1.v = 8.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [texture-ram-unnotified]: draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t first = guest[10u * stride + 11u] & rgb;
    uint32_t second = guest[10u * stride + 31u] & rgb;
    if (first != (red & rgb) || second != (green & rgb)) {
        fprintf(stderr,
                "gpu coherence selftest [texture-ram-unnotified]: first=%06x (want %06x) "
                "second=%06x (want %06x)%s\n",
                first, red & rgb, second, green & rgb,
                second == (red & rgb) ? " -- the second draw reused the stale decode" : "");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s first=%06x second=%06x\n",
           "texture-ram-unnotified", first, second);
    coherence_reset_targets();
    return 1;
}

/* One CLUT8 texture drawn twice through the same palette bytes with a different CLUT format
 * word: the palette identity in the texture cache key covers the format, so the second draw
 * decodes again (index >> 1 picks another entry) instead of reusing the first decode. The
 * case writes GeState directly, outside a list, so it notes each write as such a writer must:
 * otherwise state_get keeps the first draw's template, texture included, and never reaches
 * the key. clut-format-list sends the same change as a CLUTFORMAT command. */
static int coherence_run_clut_format_same_palette(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t tex_base = 0x04040000u;
    const uint32_t stride = 512u;
    const uint32_t rgb = 0x00FFFFFFu;
    const uint32_t index = 6u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    uint8_t *tex = (uint8_t *)SR_HOST(tex_base);

    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    memset(tex, (int)index, 16u * 16u);
    sr_gpu_vram_dirty(tex_base, 16u * 16u);

    wrap_state(base, stride);
    for (uint32_t j = 0; j < 256u; j++) {
        uint32_t entry = 0xFF000000u | wrap_hash(0x51u + j);
        memcpy(&s_ge->clutram[j * 4u], &entry, 4);
    }
    uint32_t want_first, want_second;
    memcpy(&want_first, &s_ge->clutram[index * 4u], 4);
    memcpy(&want_second, &s_ge->clutram[(index >> 1) * 4u], 4);
    s_ge->tex_enable = 1;
    s_ge->tex_addr = tex_base;
    s_ge->tex_bufw = 16;
    s_ge->tex_fmt = 5;       /* CLUT8 */
    s_ge->tex_w = 16;
    s_ge->tex_h = 16;
    s_ge->tex_func = 3;      /* REPLACE: the framebuffer shows the texel */
    s_ge->tex_filter = 0;    /* NEAREST */
    s_ge->clut_fmt = 3u | (0xFFu << 8);                 /* 8888, shift 0, mask 0xFF */
    ge_state_note_external_write();
    ge_set_gpu_hooks(&k_hooks);

    GeVtx p0, p1;
    const uint32_t white = 0xFFFFFFFFu;
    wrap_vtx(&p0, 10.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 26.0f, 26.0f, white); p1.u = 16.0f; p1.v = 16.0f;
    int ok = hook_sprite(&p0, &p1, 0);
    s_ge->clut_fmt = 3u | (1u << 2) | (0xFFu << 8);     /* same bytes, shift 1 */
    ge_state_note_external_write();
    wrap_vtx(&p0, 40.0f, 10.0f, white); p0.u = 0.0f; p0.v = 0.0f;
    wrap_vtx(&p1, 56.0f, 26.0f, white); p1.u = 16.0f; p1.v = 16.0f;
    ok = ok && hook_sprite(&p0, &p1, 0) && s_cur && target_readback(s_cur);
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [clut-format-same-palette]: draw or readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t first = guest[18u * stride + 18u] & rgb;
    uint32_t second = guest[18u * stride + 48u] & rgb;
    if (first != (want_first & rgb) || second != (want_second & rgb)) {
        fprintf(stderr,
                "gpu coherence selftest [clut-format-same-palette]: first=%06x (want %06x) "
                "second=%06x (want %06x)%s\n",
                first, want_first & rgb, second, want_second & rgb,
                second == (want_first & rgb)
                    ? " -- the second draw reused the first format's decode" : "");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s first=%06x second=%06x\n",
           "clut-format-same-palette", first, second);
    coherence_reset_targets();
    return 1;
}

/* The same palette change sent the way a title sends it: one list loads the palette, draws a
 * CLUT8 sprite, changes only CLUTFORMAT (shift 1), and draws again. Every command moves the
 * GE state serial, so the second sprite must rebuild its state and decode through index >> 1. */
static int coherence_run_clut_format_list(void) {
    const uint32_t base = 0x04000000u | 0x00160000u;
    const uint32_t tex_base = 0x04040000u, clut_base = 0x04041000u;
    const uint32_t stride = 512u, rgb = 0x00FFFFFFu, index = 6u;
    const uint32_t vaddr = 0x00101800u, list_addr = 0x00100800u;
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    coherence_reset_targets();
    memset(guest, 0, stride * FB_H * 4u);
    sr_gpu_vram_dirty(base, stride * FB_H * 4u);
    memset(SR_HOST(tex_base), (int)index, 16u * 16u);
    sr_gpu_vram_dirty(tex_base, 16u * 16u);
    uint32_t *clut = (uint32_t *)SR_HOST(clut_base);
    for (uint32_t j = 0; j < 256u; j++) clut[j] = 0xFF000000u | wrap_hash(0x51u + j);
    sr_gpu_vram_dirty(clut_base, 256u * 4u);
    const uint32_t want_first = clut[index] & rgb, want_second = clut[index >> 1] & rgb;

    RawSpriteVtx *vtx = (RawSpriteVtx *)SR_HOST(vaddr);
    vtx[0] = (RawSpriteVtx){ 0.0f, 0.0f, 10.0f, 10.0f, 0.0f };
    vtx[1] = (RawSpriteVtx){ 16.0f, 16.0f, 26.0f, 26.0f, 0.0f };
    vtx[2] = (RawSpriteVtx){ 0.0f, 0.0f, 40.0f, 10.0f, 0.0f };
    vtx[3] = (RawSpriteVtx){ 16.0f, 16.0f, 56.0f, 26.0f, 0.0f };
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    int p = 0;
    #define DL_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(val) & 0x00FFFFFFu)
    DL_EMIT(0x10, 0);
    DL_EMIT(0x9C, base & 0x00FFFFFFu);
    DL_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    DL_EMIT(0xD2, 3);
    DL_EMIT(0xD3, 0);
    DL_EMIT(0x23, 0);
    DL_EMIT(0x22, 0);
    DL_EMIT(0x21, 0);
    DL_EMIT(0xD4, 0);
    DL_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    DL_EMIT(0xA0, tex_base & 0x00FFFFFFu);
    DL_EMIT(0xA8, 16u | ((tex_base & 0xFF000000u) >> 8));
    DL_EMIT(0xB8, (4u << 8) | 4u);
    DL_EMIT(0xC2, 0);
    DL_EMIT(0xC3, 5);                                   /* CLUT8 */
    DL_EMIT(0xC6, 0);                                   /* NEAREST */
    DL_EMIT(0xC7, 0);
    DL_EMIT(0xC9, 3u | (1u << 8));                      /* REPLACE, RGBA */
    DL_EMIT(0xB0, clut_base & 0x00FFFFFFu);
    DL_EMIT(0xB1, (clut_base & 0xFF000000u) >> 8);
    DL_EMIT(0xC5, 3u | (0xFFu << 8));                   /* 8888, shift 0, mask 0xFF */
    DL_EMIT(0xC4, 32u);                                 /* 256 entries x 4 bytes */
    DL_EMIT(0x1E, 1);
    DL_EMIT(0x12, (3u << 0) | (3u << 7) | (1u << 23));
    DL_EMIT(0x01, vaddr & 0x00FFFFFFu);
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0xC5, 3u | (1u << 2) | (0xFFu << 8));       /* same palette bytes, shift 1 */
    DL_EMIT(0x01, (vaddr + 2u * (uint32_t)sizeof(RawSpriteVtx)) & 0x00FFFFFFu);
    DL_EMIT(0x04, (6u << 16) | 2u);
    DL_EMIT(0x0F, 0);
    DL_EMIT(0x0C, 0);
    #undef DL_EMIT
    ge_set_gpu_hooks(&k_hooks);
    ge_run_list(list_addr, 0);
    GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                               .width = 480, .height = 272 };
    if (!gegpu_sync_guest_fb(&desc)) {
        fprintf(stderr, "gpu coherence selftest [clut-format-list]: GPU readback failed\n");
        coherence_reset_targets();
        return 0;
    }
    uint32_t first = guest[18u * stride + 18u] & rgb;
    uint32_t second = guest[18u * stride + 48u] & rgb;
    coherence_reset_targets();
    if (first != want_first || second != want_second) {
        fprintf(stderr,
                "gpu coherence selftest [clut-format-list]: first=%06x (want %06x) "
                "second=%06x (want %06x)%s\n",
                first, want_first, second, want_second,
                second == want_first ? " -- the second draw reused the first format's decode" : "");
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s first=%06x second=%06x\n",
           "clut-format-list", first, second);
    return 1;
}

/* ---- blend parity: Vulkan vs ge.c (arbiter) vs the R6 colour product --------------------- */

#define PARITY_GRID 16u                  /* 16 x 16 sprites, 4x4 pixels each */
#define PARITY_ORIGIN 64u
#define PARITY_FF_BOUND 2                /* analytic bound for truncating vs rounding blends */

static uint32_t parity_src(uint32_t sprite) { return wrap_hash(0x51u + sprite * 2654435761u); }
static uint32_t parity_bg(uint32_t idx) { return wrap_hash(0xB6u + idx * 40503u); }

static int parity_clamp8(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

/* ge.c factor_component(): the blend factor for one channel, integer 0..255. */
static int parity_factor(int f, int src_side, int chan, const int s[3], int sa,
                         const int d[3], int da, uint32_t fixa, uint32_t fixb) {
    int sv = s[chan], dv = d[chan];
    int fixed_a = (int)((fixa >> (8 * chan)) & 0xFFu), fixed_b = (int)((fixb >> (8 * chan)) & 0xFFu);
    switch (f & 0xF) {
        case 0: return src_side ? dv : sv;
        case 1: return 255 - (src_side ? dv : sv);
        case 2: return sa;
        case 3: return 255 - sa;
        case 4: return da;
        case 5: return 255 - da;
        case 6: return sa * 2;          /* doubled alphas are not clamped (PSP-GE-RASTER-001) */
        case 7: return 255 - sa * 2;
        case 8: return da * 2;
        case 9: return 255 - da * 2;
        default: return src_side ? fixed_a : fixed_b;
    }
}

/* The same blend equation with the R6 colour product ((2x+1)(2s+1))>>10 for every
 * multiply (ge_raster_ref.c sr_ge_color_mul for factors 0..255), extended to the doubled
 * alphas' 0..510 with the factor's sign applied afterwards (PSP-GE-RASTER-001). Min, max and
 * absdiff ignore the factors and equations 6 and 7 pass the source through (as ge.c does). */
static int parity_r6_mul(int v, int f) {
    int m = ((2 * v + 1) * (2 * (f < 0 ? -f : f) + 1)) >> 10;
    return f < 0 ? -m : m;
}

static int parity_r6_chan(int s, int d, int sf, int df, int eq) {
    int sv = parity_r6_mul(s, sf);
    int dv = parity_r6_mul(d, df);
    switch (eq & 7) {
        case 1: return parity_clamp8(sv - dv);
        case 2: return parity_clamp8(dv - sv);
        case 3: return s < d ? s : d;
        case 4: return s > d ? s : d;
        case 5: return s > d ? s - d : d - s;
        case 6: case 7: return s;
        default: return parity_clamp8(sv + dv);
    }
}

/* FIX blend constants for every factor/equation cell: two distinct RGB triples whose
 * channels all differ, so a dual-FIX state (sfp and dfp both FIX, constants unequal) is
 * exercised and a channel swap or a constant mix-up changes the picture. */
#define PARITY_FIXA 0x3F7F9Fu
#define PARITY_FIXB 0x7F5F2Fu

static uint32_t parity_list(uint32_t base, uint32_t vaddr, uint32_t blend_mode, uint32_t fixa,
                            uint32_t fixb, uint32_t sprites) {
    const uint32_t list_addr = 0x00100000u;
    uint32_t *dl = (uint32_t *)SR_HOST(list_addr);
    uint32_t stride = 512u;
    int p = 0;
    #define PARITY_EMIT(cmd, val) dl[p++] = ((uint32_t)(cmd) << 24) | ((val) & 0x00FFFFFFu)
    PARITY_EMIT(0x10, 0);
    PARITY_EMIT(0x4C, 0);
    PARITY_EMIT(0x9C, base & 0x00FFFFFFu);
    PARITY_EMIT(0x9D, stride | ((base & 0xFF000000u) >> 8));
    PARITY_EMIT(0xD2, 3);
    PARITY_EMIT(0xD3, 0);
    PARITY_EMIT(0x1D, 0);
    PARITY_EMIT(0x1E, 0);
    PARITY_EMIT(0x1F, 0);
    PARITY_EMIT(0x20, 0);
    PARITY_EMIT(0x22, 0);
    PARITY_EMIT(0x23, 0);
    PARITY_EMIT(0xE7, 1);
    PARITY_EMIT(0xE8, 0);
    PARITY_EMIT(0xE9, 0);
    PARITY_EMIT(0x15, 0);
    PARITY_EMIT(0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    PARITY_EMIT(0xD4, 0);
    PARITY_EMIT(0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    PARITY_EMIT(0x21, 1);
    PARITY_EMIT(0xDF, blend_mode & 0xFFFu);
    PARITY_EMIT(0xE0, fixa);
    PARITY_EMIT(0xE1, fixb);
    /* 8888 colour, float position, transform bypass (through) */
    PARITY_EMIT(0x12, (7u << 2) | (3u << 7) | (1u << 23));
    PARITY_EMIT(0x01, vaddr & 0x00FFFFFFu);
    PARITY_EMIT(0x04, (6u << 16) | (sprites * 2u));
    PARITY_EMIT(0x0F, 0);
    PARITY_EMIT(0x0C, 0);
    #undef PARITY_EMIT
    return list_addr;
}

/* Guest vertex layout for VERTEXTYPE 8888 colour + float position: colour first. */
typedef struct ParityGuestVtx { uint32_t rgba; float x, y, z; } ParityGuestVtx;

static void parity_vertices(uint32_t vaddr, uint32_t sprites) {
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(vaddr);
    for (uint32_t i = 0; i < sprites; i++) {
        uint32_t gx = i % PARITY_GRID, gy = i / PARITY_GRID;
        float x0 = (float)(PARITY_ORIGIN + 4u * gx), y0 = (float)(PARITY_ORIGIN + 4u * gy);
        uint32_t c = parity_src(i);
        v[2 * i]     = (ParityGuestVtx){ c, x0, y0, 0.0f };
        v[2 * i + 1] = (ParityGuestVtx){ c, x0 + 4.0f, y0 + 4.0f, 0.0f };
    }
}

static void parity_fill_background(uint32_t base) {
    uint32_t *guest = (uint32_t *)SR_HOST(base);
    for (uint32_t y = 0; y < FB_H; y++)
        for (uint32_t x = 0; x < FB_W; x++) guest[y * FB_W + x] = parity_bg(y * FB_W + x);
    sr_gpu_vram_dirty(base, FB_W * FB_H * 4u);
}

typedef struct ParityTally {
    unsigned combos, pixels_nonzero;
    unsigned max_gpu_vs_sw, max_gpu_vs_r6, max_sw_vs_r6;
} ParityTally;

static int coherence_run_blend_parity(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t vaddr = 0x00101000u;
    const uint32_t stride = 512u;
    const uint32_t sprites = PARITY_GRID * PARITY_GRID;
    const uint32_t bytes = FB_W * FB_H * 4u;
    uint32_t *gpu_out = (uint32_t *)malloc(bytes);
    if (!gpu_out) return 0;
    ParityTally shader = {0}, ff = {0};
    int ok = 1;

    parity_vertices(vaddr, sprites);
    for (uint32_t eq = 0; eq < 8 && ok; eq++) {
        for (uint32_t sfp = 0; sfp <= 10 && ok; sfp++) {
            for (uint32_t dfp = 0; dfp <= 10 && ok; dfp++) {
                const uint32_t fixa = PARITY_FIXA, fixb = PARITY_FIXB;
                const uint32_t mode = sfp | (dfp << 4) | (eq << 8);
                const uint32_t list = parity_list(base, vaddr, mode, fixa, fixb, sprites);
                /* the matrix draws onto an 8888 framebuffer with dithering off, so
                 * build_state's routing reduces to blend_needs_shader() */
                int shader_path = blend_needs_shader(sfp, dfp, eq, fixa, fixb);
                ParityTally *tally = shader_path ? &shader : &ff;

                /* Vulkan path */
                coherence_reset_targets();
                parity_fill_background(base);
                ge_set_gpu_hooks(&k_hooks);
                ge_run_list(list, 0);
                GeGpuFbDescriptor desc = { .addr = base, .format = 3, .stride = stride,
                                           .width = 480, .height = FB_H };
                if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) { ok = 0; break; }
                memcpy(gpu_out, SR_HOST(base), bytes);

                /* ge.c software path (the arbiter) */
                coherence_reset_targets();
                parity_fill_background(base);
                ge_set_gpu_hooks(NULL);
                ge_run_list(list, 0);
                ge_set_gpu_hooks(&k_hooks);
                const uint32_t *sw_out = (const uint32_t *)SR_HOST(base);

                unsigned max_gs = 0, max_gr = 0, max_sr = 0;
                for (uint32_t sprite = 0; sprite < sprites; sprite++) {
                    uint32_t sc = parity_src(sprite);
                    int s[3] = { (int)(sc & 0xFF), (int)((sc >> 8) & 0xFF), (int)((sc >> 16) & 0xFF) };
                    int sa = (int)(sc >> 24);
                    for (uint32_t py = 0; py < 4u; py++) {
                        for (uint32_t px = 0; px < 4u; px++) {
                            uint32_t idx = (PARITY_ORIGIN + 4u * (sprite / PARITY_GRID) + py) * FB_W +
                                           PARITY_ORIGIN + 4u * (sprite % PARITY_GRID) + px;
                            uint32_t dw = parity_bg(idx);
                            int d[3] = { (int)(dw & 0xFF), (int)((dw >> 8) & 0xFF), (int)((dw >> 16) & 0xFF) };
                            int da = (int)(dw >> 24);
                            uint32_t g = gpu_out[idx], w = sw_out[idx];
                            for (int ch = 0; ch < 4; ch++) {
                                int gv = (int)((g >> (8 * ch)) & 0xFF), wv = (int)((w >> (8 * ch)) & 0xFF);
                                if (abs(gv - wv) > (int)max_gs) max_gs = (unsigned)abs(gv - wv);
                                if (gv != wv) tally->pixels_nonzero++;
                            }
                            for (int ch = 0; ch < 3; ch++) {
                                int sf = parity_factor((int)sfp, 1, ch, s, sa, d, da, fixa, fixb);
                                int df = parity_factor((int)dfp, 0, ch, s, sa, d, da, fixa, fixb);
                                int ref = parity_r6_chan(s[ch], d[ch], sf, df, (int)eq);
                                int gv = (int)((g >> (8 * ch)) & 0xFF), wv = (int)((w >> (8 * ch)) & 0xFF);
                                if (abs(gv - ref) > (int)max_gr) max_gr = (unsigned)abs(gv - ref);
                                if (abs(wv - ref) > (int)max_sr) max_sr = (unsigned)abs(wv - ref);
                            }
                        }
                    }
                }
                tally->combos++;
                if (max_gs > tally->max_gpu_vs_sw) tally->max_gpu_vs_sw = max_gs;
                if (max_gr > tally->max_gpu_vs_r6) tally->max_gpu_vs_r6 = max_gr;
                if (max_sr > tally->max_sw_vs_r6) tally->max_sw_vs_r6 = max_sr;

                if ((shader_path && max_gs != 0) || (!shader_path && max_gs > PARITY_FF_BOUND)) {
                    /* first deviating pixel, with its inputs, for the failure report */
                    for (uint32_t i = 0; i < sprites * 16u; i++) {
                        uint32_t sprite = i / 16u, px = i % 4u, py = (i / 4u) % 4u;
                        uint32_t idx = (PARITY_ORIGIN + 4u * (sprite / PARITY_GRID) + py) * FB_W +
                                       PARITY_ORIGIN + 4u * (sprite % PARITY_GRID) + px;
                        if (gpu_out[idx] == sw_out[idx]) continue;
                        fprintf(stderr, "gpu coherence selftest [blend-parity]: sfp=%u dfp=%u eq=%u "
                                        "x=%u y=%u gpu=%08x ge_c=%08x src=%08x dst=%08x\n",
                                sfp, dfp, eq, idx % FB_W, idx / FB_W, gpu_out[idx], sw_out[idx],
                                parity_src(sprite), parity_bg(idx));
                        break;
                    }
                    fprintf(stderr, "gpu coherence selftest [blend-parity]: %s blend deviates from ge.c "
                                    "by %u (bound %u)\n", shader_path ? "shader" : "fixed-function",
                            max_gs, shader_path ? 0u : (unsigned)PARITY_FF_BOUND);
                    ok = 0;
                }
                coherence_reset_targets();
            }
        }
    }
    ge_set_gpu_hooks(&k_hooks);
    free(gpu_out);
    if (!ok) return 0;
    printf("gpu coherence selftest: PASS %-22s combos=%u max_delta_vs_ge_c=0 (shader blend, exact)\n",
           "blend-parity-shader", shader.combos);
    printf("gpu coherence selftest: PASS %-22s combos=%u max_delta_vs_ge_c=%u (fixed-function, bound %d)\n",
           "blend-parity-ff", ff.combos, ff.max_gpu_vs_sw, PARITY_FF_BOUND);
    printf("gpu coherence selftest: INFO r6-reference shader gpu_vs_r6=%u sw_vs_r6=%u "
           "ff gpu_vs_r6=%u sw_vs_r6=%u\n",
           shader.max_gpu_vs_r6, shader.max_sw_vs_r6, ff.max_gpu_vs_r6, ff.max_sw_vs_r6);
    return 1;
}

/* ---- submission granularity (#827, issue #323) --------------------------------------------
 *
 * SUBMIT_BATCH_MAX caps how many recorded GE-ring ops -- render batches, destination
 * snapshot copies and ring uploads -- share one vkQueueSubmit. This fixture proves the cap is
 * a submission granularity and not an ordering boundary. It builds one GE display list and
 * runs it through ge.c's interpreter and the production hooks, so nothing here reaches the
 * backend by a route the player does not take. The list runs once per cap, and every frame
 * is compared byte for byte with ge.c's software rasterizer, the reference. The list chains
 * GPU-only dependencies that would change pixels if a longer command buffer lost or
 * reordered one:
 *   - overlapping dual-FIX shader-blend sprites with a new FIX constant per draw: each draw
 *     submits the previous render op and records a destination snapshot copy, the
 *     render+snapshot pairs behind the "mixed" player-route submits in #827;
 *   - CLUT8 sprites with a palette reload per draw (a new cache entry and a ring upload into
 *     the open command buffer each), then a GE block transfer that rewrites the texels
 *     through ge.c's CPU copy and dirty notification, and draws that re-upload those cached
 *     images in place after earlier draws in the same command buffer sampled them;
 *   - render-to-texture: draws alternate between a second target and the main one, which
 *     samples the second target's newest pixels directly.
 * No CPU-visible boundary falls inside the list, so the cap is the only thing that splits
 * it: a run must cost exactly ops/cap submits and leave ops%cap ops open. That also pins
 * SR_GPU_SUBMIT_BATCH=8 to the legacy schedule, op for op. */
#define SB_FBA_A    0x00000000u            /* main target (VRAM offset) */
#define SB_FBA_B    0x00088000u            /* render-to-texture source, after A's rows */
#define SB_ZBA      0x00110000u
#define SB_LIST     0x08400000u            /* synthetic main RAM from here on */
#define SB_VTX      0x08480000u
#define SB_PAL      0x08500000u            /* 16-entry 8888 palettes, 64 bytes each */
#define SB_TEX      0x08510000u            /* 16x16 CLUT8 texels, one 256-byte copy per run */
#define SB_XFER_SRC 0x08520000u            /* the block transfer's replacement texels */
#define SB_FIXB     0x5F7F3Fu

typedef struct SubmitBatchShape {
    uint32_t blend_draws;                  /* overlapping shader-blend sprites */
    uint32_t spaced_draws;                 /* shader-blend sprites in disjoint cells */
    uint32_t tail_draws;                   /* shader-blend sprites over the CLUT sprites */
    uint32_t clut_draws;                   /* CLUT8 sprites, one palette reload each */
    uint32_t rewrite_draws;                /* CLUT8 sprites after the block-transfer rewrite */
    uint32_t rtt_rounds;                   /* draw into B, then sample B from A */
    uint32_t size;                         /* shader-blend and target-B sprite edge */
} SubmitBatchShape;

typedef struct SubmitBatchList { uint32_t *dl; uint32_t n; uint32_t voff; } SubmitBatchList;

static void sb_emit(SubmitBatchList *l, uint32_t cmd, uint32_t val) {
    l->dl[l->n++] = (cmd << 24) | (val & 0x00FFFFFFu);
}

static void sb_target(SubmitBatchList *l, uint32_t fba) {
    sb_emit(l, 0x9C, fba);
    sb_emit(l, 0x9D, 512u | (0x04000000u >> 8));
}

/* One through-mode sprite: its two vertices, then VADDR and PRIM. Untextured sprites use
 * the 8888-colour layout (ParityGuestVtx), textured ones float UV + position. */
static void sb_sprite(SubmitBatchList *l, int textured, float x, float y, float u, float v,
                      float size, uint32_t rgba) {
    uint32_t vaddr = SB_VTX + l->voff;
    if (textured) {
        RawSpriteVtx *q = (RawSpriteVtx *)SR_HOST(vaddr);
        q[0] = (RawSpriteVtx){ u, v, x, y, 0.0f };
        q[1] = (RawSpriteVtx){ u + size, v + size, x + size, y + size, 0.0f };
        l->voff += 2u * (uint32_t)sizeof(RawSpriteVtx);
    } else {
        ParityGuestVtx *q = (ParityGuestVtx *)SR_HOST(vaddr);
        q[0] = (ParityGuestVtx){ rgba, x, y, 0.0f };
        q[1] = (ParityGuestVtx){ rgba, x + size, y + size, 0.0f };
        l->voff += 2u * (uint32_t)sizeof(ParityGuestVtx);
    }
    sb_emit(l, 0x01, vaddr);
    sb_emit(l, 0x04, (6u << 16) | 2u);
}

static uint32_t sb_fixa(uint32_t i) {
    uint32_t a = wrap_hash(0x3Du + i * 2654435761u) & 0xFFFFFFu;
    return a == SB_FIXB ? a ^ 1u : a;      /* dual FIX needs two distinct constants */
}

/* Write fresh inputs (both targets, texels, palettes, transfer source) and the display
 * list; return the list address. `tex` lets each run own its texture cache entries. */
static uint32_t submit_batch_prepare(const SubmitBatchShape *sh, uint32_t tex) {
    const uint32_t bytes = FB_W * FB_H * 4u;
    uint32_t *a = (uint32_t *)SR_HOST(0x04000000u | SB_FBA_A);
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) a[i] = wrap_pixel_rgba(0xA5u, i);
    sr_gpu_vram_dirty(0x04000000u | SB_FBA_A, bytes);
    memset(SR_HOST(0x04000000u | SB_FBA_B), 0, bytes);
    sr_gpu_vram_dirty(0x04000000u | SB_FBA_B, bytes);
    uint8_t *texels = (uint8_t *)SR_HOST(tex), *repl = (uint8_t *)SR_HOST(SB_XFER_SRC);
    for (uint32_t v = 0; v < 16u; v++) {
        for (uint32_t u = 0; u < 16u; u++) {
            texels[v * 16u + u] = (uint8_t)((u * 7u + v * 3u) & 15u);
            repl[v * 16u + u] = (uint8_t)((u * 5u + v * 11u + 9u) & 15u);
        }
    }
    sr_gpu_vram_dirty(tex, 256u);
    uint32_t *pal = (uint32_t *)SR_HOST(SB_PAL);
    for (uint32_t k = 0; k < sh->clut_draws; k++)
        for (uint32_t j = 0; j < 16u; j++) pal[k * 16u + j] = wrap_pixel_rgba(0xC1u + k, j);

    const uint32_t colour = (7u << 2) | (3u << 7) | (1u << 23);   /* 8888, float pos, through */
    const uint32_t uv = (3u << 0) | (3u << 7) | (1u << 23);       /* float UV, float pos */
    const float size = (float)sh->size;
    const uint32_t span_x = (uint32_t)DRAW_W - sh->size - 32u, span_y = FB_H - sh->size - 32u;
    SubmitBatchList l = { (uint32_t *)SR_HOST(SB_LIST), 0u, 0u };
    sb_emit(&l, 0x10, (SB_VTX >> 8) & 0x000F0000u);   /* BASE: vertices live in main RAM */
    sb_emit(&l, 0x13, 0);
    sb_emit(&l, 0x4C, 0);
    sb_emit(&l, 0x4D, 0);
    sb_target(&l, SB_FBA_A);
    sb_emit(&l, 0x9E, SB_ZBA);
    sb_emit(&l, 0x9F, 512u | (0x04000000u >> 8));
    sb_emit(&l, 0xD2, 3);
    sb_emit(&l, 0xD3, 0);
    static const uint8_t k_off[] = { 0x1D, 0x1F, 0x20, 0x22, 0x23, 0x24, 0x27, 0x28, 0xE8, 0xE9 };
    for (size_t i = 0; i < sizeof(k_off); i++) sb_emit(&l, k_off[i], 0);
    sb_emit(&l, 0xE7, 1);
    sb_emit(&l, 0x15, 0);
    sb_emit(&l, 0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    sb_emit(&l, 0xD4, 0);
    sb_emit(&l, 0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    sb_emit(&l, 0xC0, 0);
    sb_emit(&l, 0xC2, 0);
    sb_emit(&l, 0xC5, 3u | (0xFFu << 8));            /* 8888 palette, mask 0xFF */
    sb_emit(&l, 0xC6, 0);
    sb_emit(&l, 0xC7, 0);
    sb_emit(&l, 0xC9, 3u | (1u << 8));               /* replace, texture alpha */

    /* Shader blend: FIXA*src + FIXB*dst with a new FIXA per draw, each sprite overlapping
     * the last, so every draw blends against the previous draw's result. */
    sb_emit(&l, 0x1E, 0);
    sb_emit(&l, 0x21, 1);
    sb_emit(&l, 0xDF, 10u | (10u << 4));
    sb_emit(&l, 0xE1, SB_FIXB);
    sb_emit(&l, 0x12, colour);
    for (uint32_t i = 0; i < sh->blend_draws; i++) {
        sb_emit(&l, 0xE0, sb_fixa(i));
        sb_sprite(&l, 0, (float)(16u + (i * 3u) % span_x), (float)(16u + (i * 2u) % span_y),
                  0.0f, 0.0f, size, wrap_pixel_rgba(0x5Au, i));
    }

    /* Shader-blend sprites in disjoint cells of a band below and to the right of the
     * overlapping ones: nothing drawn since the last snapshot copy overlaps any of them (the
     * drawn region is tracked as one bounding rectangle, so each cell also lies outside the
     * rectangle around everything before it), and each reads the existing snapshot instead
     * of copying the target again. */
    for (uint32_t k = 0; k < sh->spaced_draws; k++) {
        sb_emit(&l, 0xE0, sb_fixa(sh->blend_draws + k));
        sb_sprite(&l, 0, (float)(200u + k * 24u), 228.0f, 0.0f, 0.0f, 12.0f,
                  wrap_pixel_rgba(0x6Bu, k));
    }

    /* CLUT8 sprites, one palette per draw, overlapping one another. */
    sb_emit(&l, 0x21, 0);
    sb_emit(&l, 0x1E, 1);
    sb_emit(&l, 0xA0, tex);
    sb_emit(&l, 0xA8, 16u | ((tex & 0xFF000000u) >> 8));
    sb_emit(&l, 0xB8, 4u | (4u << 8));
    sb_emit(&l, 0xC3, 5);
    sb_emit(&l, 0xB1, (SB_PAL & 0xFF000000u) >> 8);
    sb_emit(&l, 0x12, uv);
    for (uint32_t k = 0; k < sh->clut_draws; k++) {
        sb_emit(&l, 0xB0, SB_PAL + k * 64u);
        sb_emit(&l, 0xC4, 2);
        sb_sprite(&l, 1, (float)(240u + (k * 5u) % 200u), (float)(140u + (k * 3u) % 100u),
                  0.0f, 0.0f, 16.0f, 0);
    }
    /* Rewrite the texels with a 16-bit block transfer (16 rows of 16 bytes), then draw with
     * earlier palettes: same cache keys, new contents, so the images update in place. */
    sb_emit(&l, 0xB2, SB_XFER_SRC & 0x00FFFFF0u);
    sb_emit(&l, 0xB3, 8u | ((SB_XFER_SRC & 0xFF000000u) >> 8));
    sb_emit(&l, 0xB4, tex & 0x00FFFFF0u);
    sb_emit(&l, 0xB5, 8u | ((tex & 0xFF000000u) >> 8));
    sb_emit(&l, 0xEB, 0);
    sb_emit(&l, 0xEC, 0);
    sb_emit(&l, 0xEE, 7u | (15u << 10));
    sb_emit(&l, 0xEA, 0);
    for (uint32_t j = 0; j < sh->rewrite_draws; j++) {
        sb_emit(&l, 0xB0, SB_PAL + (j % sh->clut_draws) * 64u);
        sb_emit(&l, 0xC4, 2);
        sb_sprite(&l, 1, (float)(244u + j * 6u), (float)(150u + j * 4u), 0.0f, 0.0f, 16.0f, 0);
    }

    /* Shader-blend sprites over the first CLUT sprites, which were drawn after the last
     * snapshot copy: each needs a fresh copy although no blended draw came between. */
    sb_emit(&l, 0x1E, 0);
    sb_emit(&l, 0x21, 1);
    sb_emit(&l, 0x12, colour);
    for (uint32_t t = 0; t < sh->tail_draws; t++) {
        sb_emit(&l, 0xE0, sb_fixa(sh->blend_draws + sh->spaced_draws + t));
        sb_sprite(&l, 0, (float)(236u + t * 9u), 136.0f + (float)t, 0.0f, 0.0f, 24.0f,
                  wrap_pixel_rgba(0x7Cu, t));
    }
    sb_emit(&l, 0x21, 0);

    /* Render-to-texture: draw into B, then sample B's newest sprite (and its border) from A. */
    for (uint32_t r = 0; r < sh->rtt_rounds; r++) {
        uint32_t bx = 8u + (r * 12u) % span_x, by = 8u + (r * 6u) % span_y;
        sb_target(&l, SB_FBA_B);
        sb_emit(&l, 0x1E, 0);
        sb_emit(&l, 0x12, colour);
        sb_sprite(&l, 0, (float)bx, (float)by, 0.0f, 0.0f, size, wrap_pixel_rgba(0xB0u, r));
        sb_target(&l, SB_FBA_A);
        sb_emit(&l, 0x1E, 1);
        sb_emit(&l, 0xA0, SB_FBA_B);
        sb_emit(&l, 0xA8, 512u | (0x04000000u >> 8));
        sb_emit(&l, 0xB8, 9u | (9u << 8));
        sb_emit(&l, 0xC3, 3);
        sb_emit(&l, 0x12, uv);
        sb_sprite(&l, 1, (float)(24u + (r * 20u) % span_x), (float)(160u + (r % 4u) * 8u),
                  (float)(bx - 4u), (float)(by - 4u), size + 8.0f, 0);
    }
    sb_emit(&l, 0x0F, 0);
    sb_emit(&l, 0x0C, 0);
    return SB_LIST;
}

typedef struct SubmitBatchRun {
    unsigned long long ops, submits, waits, open, snapshots, uploads, wraps, fallbacks;
    unsigned long long snapshot_px;        /* device pixels the snapshot copies moved */
    double submit_ms, wait_ms, wall_ms;
} SubmitBatchRun;

/* Run the list at `cap` through the production hooks, then settle both targets through the
 * explicit snapshot boundary, which leaves the GPU frames in guest VRAM. */
static int submit_batch_run(const SubmitBatchShape *sh, uint32_t tex, uint32_t cap,
                            SubmitBatchRun *out) {
    memset(out, 0, sizeof(*out));
    coherence_reset_targets();
    if (!cmd_drain(GEGPU_BOUNDARY_LIFETIME)) return 0;
    uint32_t saved_cap = s_submit_batch_ops;
    if (s_async_submit) s_submit_batch_ops = cap;  /* synchronous mode is one op per submit */
    uint32_t list = submit_batch_prepare(sh, tex);
    ge_set_gpu_hooks(&k_hooks);

    GeGpuReplayStats before, after;
    unsigned long renders = s_cnt_submit;
    gegpu_replay_stats_get(&before);
    uint64_t started = SDL_GetTicksNS();
    ge_run_list(list, 0);
    gegpu_replay_stats_get(&after);
    renders = s_cnt_submit - renders;        /* render ops recorded by the list itself */
    out->open = s_cmd_slot ? s_cmd_slot->recorded_ops : 0u;
    GeGpuFbDescriptor da = { 0x04000000u | SB_FBA_A, 3u, 512u, (uint32_t)DRAW_W, FB_H };
    GeGpuFbDescriptor db = { 0x04000000u | SB_FBA_B, 3u, 512u, (uint32_t)DRAW_W, FB_H };
    int ok = gegpu_sync_guest_fb(&da) == GEGPU_SYNC_OK && gegpu_sync_guest_fb(&db) == GEGPU_SYNC_OK;
    out->wall_ms = (double)(SDL_GetTicksNS() - started) / 1000000.0;
    s_submit_batch_ops = saved_cap;

    out->snapshots = after.snapshot_copies - before.snapshot_copies;
    out->snapshot_px = after.snapshot_copy_px - before.snapshot_copy_px;
    out->uploads = after.upload_ring_reservations - before.upload_ring_reservations;
    out->wraps = after.upload_ring_wraps - before.upload_ring_wraps;
    out->fallbacks = after.upload_ring_fallbacks - before.upload_ring_fallbacks;
    out->ops = (unsigned long long)renders + out->snapshots + out->uploads + out->fallbacks;
    out->submits = after.queue_submits - before.queue_submits;
    for (int i = 0; i < GEGPU_BOUNDARY_COUNT; i++) {
        out->waits += after.boundary[i].waits - before.boundary[i].waits;
        out->submit_ms += (double)(after.boundary[i].submit_ns - before.boundary[i].submit_ns) / 1e6;
        out->wait_ms += (double)(after.boundary[i].wait_ns - before.boundary[i].wait_ns) / 1e6;
    }
    if (!ok) fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u snapshot sync failed\n", cap);
    return ok;
}

static int submit_batch_same(const char *what, uint32_t cap, const uint32_t *gpu,
                             const uint32_t *ref) {
    for (uint32_t i = 0; i < (uint32_t)(FB_W * FB_H); i++) {
        if (gpu[i] != ref[i]) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u target %s x=%u y=%u "
                            "gpu=%08x ge_c=%08x\n", cap, what, i % FB_W, i / FB_W, gpu[i], ref[i]);
            return 0;
        }
    }
    return 1;
}

static int coherence_run_submit_batch(void) {
    const SubmitBatchShape sh = { 48u, 8u, 2u, 24u, 6u, 8u, 24u };
    const size_t bytes = (size_t)FB_W * FB_H * 4u;
    uint32_t *ref_a = (uint32_t *)malloc(bytes), *ref_b = (uint32_t *)malloc(bytes);
    if (!ref_a || !ref_b) {
        free(ref_a); free(ref_b);
        fprintf(stderr, "gpu coherence selftest [submit-batch]: out of memory\n");
        return 0;
    }
    /* ge.c's software rasterizer on the same list: the reference every cap must equal. */
    coherence_reset_targets();
    ge_set_gpu_hooks(NULL);
    ge_run_list(submit_batch_prepare(&sh, SB_TEX), 0);
    ge_set_gpu_hooks(&k_hooks);
    memcpy(ref_a, SR_HOST(0x04000000u | SB_FBA_A), bytes);
    memcpy(ref_b, SR_HOST(0x04000000u | SB_FBA_B), bytes);

    /* Legacy cap, the configured cap, and the maximum. A warm-up run first creates the
     * pipelines, depth image and targets so every measured run records the same ops. */
    const uint32_t wanted[3] = { 8u, s_submit_batch_ops, SUBMIT_BATCH_MAX };
    uint32_t caps[4] = { s_submit_batch_ops, 0u, 0u, 0u };   /* caps[0] is the warm-up */
    uint32_t ncaps = 0;
    for (uint32_t i = 0; i < (s_async_submit ? 3u : 1u); i++) {
        /* SR_GPU_SYNC_SUBMIT=1 fixes the cap at 1: one op per submit, nothing to compare */
        uint32_t cap = s_async_submit ? wanted[i] : s_submit_batch_ops;
        int dup = 0;
        for (uint32_t j = 1; j <= ncaps; j++) dup |= caps[j] == cap;
        if (!dup) caps[++ncaps] = cap;
    }
    int ok = 1;
    for (uint32_t i = 0; i <= ncaps && ok; i++) {
        SubmitBatchRun run;
        uint32_t cap = caps[i];
        ok = submit_batch_run(&sh, SB_TEX + 0x100u * (i + 1u), cap, &run) &&
             submit_batch_same("A", cap, (const uint32_t *)SR_HOST(0x04000000u | SB_FBA_A), ref_a) &&
             submit_batch_same("B", cap, (const uint32_t *)SR_HOST(0x04000000u | SB_FBA_B), ref_b);
        if (!ok || i == 0) continue;         /* the warm-up checks frames only */
        /* Non-vacuous: every overlapping and tail blend draw recorded its snapshot copy, no
         * spaced blend draw did, and every palette and rewrite draw uploaded through the GE
         * chain. */
        if (run.snapshots != sh.blend_draws + sh.tail_draws ||
            run.uploads + run.fallbacks < sh.clut_draws + sh.rewrite_draws) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u snapshots=%llu uploads=%llu "
                            "fallbacks=%llu, want %u snapshots and >= %u uploads\n", cap,
                    run.snapshots, run.uploads, run.fallbacks, sh.blend_draws + sh.tail_draws,
                    sh.clut_draws + sh.rewrite_draws);
            ok = 0;
            break;
        }
        /* The overlapping blend draws find the snapshot holding this target with only the
         * previous sprite dirty, so each copies that box, not the frame: the copies move less
         * than one full frame apiece. Their frames still equal ge.c above. */
        const unsigned long long full_px = (unsigned long long)SCL_W * SCL_H;
        if (run.snapshot_px == 0u || run.snapshot_px >= run.snapshots * full_px) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u snapshot copies moved "
                            "%llu px for %llu copies of %llu px: no dirty-box copy\n",
                    cap, run.snapshot_px, run.snapshots, full_px);
            ok = 0;
            break;
        }
        /* The cap is the only thing that splits this list. A ring that wraps or is disabled
         * (SR_GPU_XFER_RING_KB, large SR_GPU_SCALE) adds boundaries of its own, so the exact
         * count is checked only where none occurred. */
        const char *accounting = "unchecked(ring wrap/fallback)";
        if (!s_async_submit) {
            accounting = "exact";
            ok = run.submits == run.ops && run.open == 0;
        } else if (!run.wraps && !run.fallbacks) {
            accounting = "exact";
            ok = run.submits == run.ops / cap && run.open == run.ops % cap;
        }
        if (!ok) {
            fprintf(stderr, "gpu coherence selftest [submit-batch]: cap=%u ops=%llu submits=%llu open=%llu, "
                            "want submits=%llu open=%llu\n", cap, run.ops, run.submits, run.open,
                    s_async_submit ? run.ops / cap : run.ops, s_async_submit ? run.ops % cap : 0ull);
            break;
        }
        printf("gpu coherence selftest: PASS %-22s cap=%u ops=%llu submits=%llu open=%llu waits=%llu "
               "submit_ms=%.3f wait_ms=%.3f wall_ms=%.3f accounting=%s frames=ge.c-exact\n",
               "submit-batch", cap, run.ops, run.submits, run.open, run.waits, run.submit_ms,
               run.wait_ms, run.wall_ms, accounting);
    }
    free(ref_a);
    free(ref_b);
    coherence_reset_targets();
    return ok;
}

static int coherence_queue_flat_sprite(uint32_t base, uint32_t stride, uint32_t fmt,
                                       float x0, float y0, float x1, float y1) {
    memset(s_ge, 0, sizeof(*s_ge));
    s_ge->fbp = base;
    s_ge->fbw = stride;
    s_ge->fbfmt = fmt;
    s_ge->zbp = 0x04080000u;
    s_ge->zbw = stride;
    s_ge->scis_x2 = (int)FB_W - 1;
    s_ge->scis_y2 = (int)FB_H - 1;
    s_ge->maxz = 65535u;
    GeVtx p0 = { x0, y0, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0x40, 0x50, 0x60, 0xff };
    GeVtx p1 = p0;
    p1.x = x1;
    p1.y = y1;
    return hook_sprite(&p0, &p1, 0) && s_nbatch;
}

/* #1170: a title's own memmove/memset replaced by a native codegen stub
 * (sr_native_memmove/sr_native_memset) must keep the scalar accessors' VRAM
 * contract.  The HST HUD copies a rendered 5551 framebuffer rect into a RAM
 * texture right after the list ends; a stub that formed its host pointer
 * without the read prepare copied whatever stale bytes guest VRAM held.  A
 * fill that skipped the write commit was later overwritten by the stale GPU
 * target on the next readback. */
static int coherence_run_native_bulk_copy(void) {
    const uint32_t fba = 0x00100000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u, fmt = 1u;
    const uint32_t ram = 0x08020000u;
    const uint32_t rect = 8u;
    const uint16_t want = (uint16_t)(fb_pack(0x00605040u, fmt) & 0x7fffu);
    coherence_reset_targets();
    g_sr_vram_read_pending = 0;
    memset(SR_HOST(base), 0, stride * FB_H * 2u);
    memset(SR_HOST(ram), 0xEE, rect * rect * 2u);
    if (!coherence_queue_flat_sprite(base, stride, fmt, 10.0f, 10.0f,
                                     10.0f + (float)rect, 10.0f + (float)rect)) {
        fprintf(stderr, "gpu coherence selftest: native-bulk failed to queue sprite\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_flush("listend");
    if (!g_sr_vram_read_pending) {
        fprintf(stderr, "gpu coherence selftest: native-bulk list end missed pending ownership\n");
        coherence_reset_targets();
        return 0;
    }
    for (uint32_t row = 0; row < rect; row++)
        sr_native_memmove(ram + row * rect * 2u, base + ((10u + row) * stride + 10u) * 2u,
                          rect * 2u, 0u);
    for (uint32_t i = 0; i < rect * rect; i++) {
        uint16_t px;
        memcpy(&px, SR_HOST(ram + i * 2u), sizeof px);
        if ((px & 0x7fffu) != want) {
            fprintf(stderr,
                    "gpu coherence selftest: native-bulk memmove pixel %u copied 0x%04x, expected 0x%04x\n",
                    i, px, want);
            coherence_reset_targets();
            return 0;
        }
    }

    const uint32_t fill = base + (12u * stride + 12u) * 2u;
    sr_native_memset(fill, 0xABu, 4u, 0u);
    if (!coherence_queue_flat_sprite(base, stride, fmt, 100.0f, 100.0f, 104.0f, 104.0f)) {
        fprintf(stderr, "gpu coherence selftest: native-bulk failed to queue second sprite\n");
        coherence_reset_targets();
        return 0;
    }
    gegpu_flush("listend");
    uint32_t filled = MEM_R32(fill);
    if (filled != 0xABABABABu) {
        fprintf(stderr,
                "gpu coherence selftest: native-bulk memset lost to the GPU target (0x%08x, expected 0xabababab)\n",
                filled);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu coherence selftest: PASS native-bulk-copy       5551 memmove rows=%u + memset survives readback\n",
           rect);
    coherence_reset_targets();
    return 1;
}

/* Stencil parity: the GPU stencil path against ge.c, bit for bit, in every framebuffer
 * format. Each cell of the matrix is one state (test function, reference, mask, the three
 * ops, the alpha write mask, blending) drawing two overlapping sprites onto a hashed
 * background, so the second sprite reads what the first wrote: a state that reads its own
 * writes must split there (stencil_splits). The depth list fills a striped depth buffer in
 * clear mode, with the stencil test on, which clear mode ignores, then draws the states the
 * GPU path takes under the depth test (depth-fail and tested stencil-fail ops KEEP). No
 * primitive may take the software fallback. */
#define STP_CELLS_X 24u
#define STP_PITCH 10u
#define STP_ORIGIN 16u

typedef struct StpList { uint32_t *dl; int p; } StpList;

static void stp_emit(StpList *l, uint32_t cmd, uint32_t val) {
    l->dl[l->p++] = (cmd << 24) | (val & 0x00FFFFFFu);
}

static void stp_begin(StpList *l, uint32_t base, uint32_t fmt, int ztest) {
    l->dl = (uint32_t *)SR_HOST(0x08040000u);
    l->p = 0;
    stp_emit(l, 0x10, 0x080000u);                         /* BASE: vertices in main RAM */
    stp_emit(l, 0x9C, base & 0x00FFFFFFu);
    stp_emit(l, 0x9D, 512u | ((base & 0xFF000000u) >> 8));
    stp_emit(l, 0x9E, 0);                                  /* depth at VRAM offset 0 */
    stp_emit(l, 0x9F, 512u | 0x040000u);
    stp_emit(l, 0xD2, fmt);
    stp_emit(l, 0xD3, 0);
    stp_emit(l, 0x1D, 0); stp_emit(l, 0x1E, 0); stp_emit(l, 0x1F, 0);
    stp_emit(l, 0x20, 0); stp_emit(l, 0x22, 0);
    stp_emit(l, 0x23, ztest ? 1u : 0u);
    stp_emit(l, 0x24, 1);                                  /* stencil test on throughout */
    stp_emit(l, 0xE7, 1);
    stp_emit(l, 0xE8, 0); stp_emit(l, 0xE9, 0);
    stp_emit(l, 0x15, 0);
    stp_emit(l, 0x16, ((FB_H - 1) << 10) | (FB_W - 1));
    stp_emit(l, 0xD4, 0);
    stp_emit(l, 0xD5, ((FB_H - 1) << 10) | (FB_W - 1));
    stp_emit(l, 0xDF, 2u | (3u << 4));                     /* src alpha, 1 - src alpha */
    stp_emit(l, 0x12, (7u << 2) | (3u << 7) | (1u << 23));
}

/* One sprite from (x0,y0) to (x1,y1) at depth z, its vertices at *vaddr. */
static void stp_sprite(StpList *l, uint32_t *vaddr, uint32_t rgba, float x0, float y0,
                       float x1, float y1, float z) {
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(*vaddr);
    v[0] = (ParityGuestVtx){ rgba, x0, y0, z };
    v[1] = (ParityGuestVtx){ rgba, x1, y1, z };
    stp_emit(l, 0x01, *vaddr & 0x00FFFFFFu);
    stp_emit(l, 0x04, (6u << 16) | 2u);
    *vaddr += 2u * (uint32_t)sizeof(ParityGuestVtx);
}

/* The two overlapping sprites of a cell, in one primitive command so they can share a batch. */
static void stp_cell_sprites(StpList *l, uint32_t *vaddr, uint32_t cell, float z) {
    float x = (float)(STP_ORIGIN + STP_PITCH * (cell % STP_CELLS_X));
    float y = (float)(STP_ORIGIN + STP_PITCH * (cell / STP_CELLS_X));
    uint32_t ca = wrap_hash(0x5713u + cell * 2u), cb = wrap_hash(0x5714u + cell * 2u);
    ParityGuestVtx *v = (ParityGuestVtx *)SR_HOST(*vaddr);
    v[0] = (ParityGuestVtx){ ca, x + 1.0f, y + 1.0f, z };
    v[1] = (ParityGuestVtx){ ca, x + 7.0f, y + 7.0f, z };
    v[2] = (ParityGuestVtx){ cb, x + 3.0f, y + 3.0f, z };
    v[3] = (ParityGuestVtx){ cb, x + 9.0f, y + 9.0f, z };
    stp_emit(l, 0x01, *vaddr & 0x00FFFFFFu);
    stp_emit(l, 0x04, (6u << 16) | 4u);
    *vaddr += 4u * (uint32_t)sizeof(ParityGuestVtx);
}

static void stp_cell_state(StpList *l, uint32_t cell, uint32_t func, uint32_t ops) {
    static const uint32_t smask[5] = { 0xFFu, 0xF0u, 0x0Fu, 0x80u, 0x81u };
    static const uint32_t amask[5] = { 0x00u, 0x00u, 0xF0u, 0x0Fu, 0x80u };
    uint32_t h = wrap_hash(0xA11u + cell);
    stp_emit(l, 0xDC, func | ((h & 0xFFu) << 8) | (smask[cell % 5u] << 16));
    stp_emit(l, 0xDD, ops);
    stp_emit(l, 0xE9, amask[(cell / 5u) % 5u]);
    stp_emit(l, 0x21, (cell & 1u) ? 1u : 0u);
}

static void stp_end(StpList *l) {
    stp_emit(l, 0x10, 0);                                  /* later lists expect BASE 0 */
    stp_emit(l, 0x0F, 0);
    stp_emit(l, 0x0C, 0);
}

/* Matrix list: every test function with every stencil-fail and depth-pass op, depth off. */
static uint32_t stp_matrix_list(uint32_t base, uint32_t fmt) {
    StpList l; stp_begin(&l, base, fmt, 0);
    uint32_t vaddr = 0x08060000u, cell = 0;
    for (uint32_t func = 0; func < 8u; func++)
        for (uint32_t sfail = 0; sfail < 6u; sfail++)
            for (uint32_t zpass = 0; zpass < 6u; zpass++, cell++) {
                stp_cell_state(&l, cell, func, sfail | (((cell / 7u) % 6u) << 8) | (zpass << 16));
                stp_cell_sprites(&l, &vaddr, cell, 0.0f);
            }
    stp_end(&l);
    return 0x08040000u;
}

/* Depth list: a striped depth buffer, then depth-tested cells. */
static uint32_t stp_depth_list(uint32_t base, uint32_t fmt) {
    static const uint32_t zfunc[4] = { 4u, 5u, 6u, 7u };   /* less, lequal, greater, gequal */
    StpList l; stp_begin(&l, base, fmt, 1);
    uint32_t vaddr = 0x08060000u;
    stp_emit(&l, 0xD3, 1u | 0x400u | 0x100u);              /* depth and colour, stencil kept */
    for (uint32_t x = 0; x < STP_CELLS_X * STP_PITCH; x += 2u)
        stp_sprite(&l, &vaddr, 0xFF808080u, (float)(STP_ORIGIN + x), (float)STP_ORIGIN,
                   (float)(STP_ORIGIN + x + 2u), (float)(STP_ORIGIN + 8u * STP_PITCH),
                   (x & 2u) ? 49152.0f : 16384.0f);
    stp_emit(&l, 0xD3, 0);
    uint32_t cell = 0;
    for (uint32_t func = 0; func < 8u; func++)
        for (uint32_t zpass = 0; zpass < 6u; zpass++, cell++) {
            stp_cell_state(&l, cell, func, zpass << 16);
            stp_emit(&l, 0xDE, zfunc[cell % 4u]);
            stp_emit(&l, 0xE7, (cell / 4u) & 1u);          /* depth writes on and off */
            stp_cell_sprites(&l, &vaddr, cell, 32768.0f);
        }
    for (uint32_t sfail = 1; sfail < 6u; sfail++, cell++) {
        stp_cell_state(&l, cell, 1u, sfail | (2u << 16));   /* always: the fail op never runs */
        stp_emit(&l, 0xDE, zfunc[cell % 4u]);
        stp_emit(&l, 0xE7, 0);
        stp_cell_sprites(&l, &vaddr, cell, 32768.0f);
    }
    stp_end(&l);
    return 0x08040000u;
}

static void stp_fill_background(uint32_t base, uint32_t fmt) {
    if (fmt == 3u) {
        uint32_t *px = (uint32_t *)SR_HOST(base);
        for (uint32_t i = 0; i < FB_W * FB_H; i++) px[i] = wrap_hash(0x3C1u + i * 40503u);
    } else {
        uint16_t *px = (uint16_t *)SR_HOST(base);
        for (uint32_t i = 0; i < FB_W * FB_H; i++)
            px[i] = (uint16_t)wrap_hash(0x3C1u + i * 40503u);
    }
    sr_gpu_vram_dirty(base, FB_W * FB_H * (fmt == 3u ? 4u : 2u));
}

static int coherence_run_stencil_parity(void) {
    const uint32_t base = 0x04000000u | 0x00140000u;
    const uint32_t max_bytes = FB_W * FB_H * 4u;
    uint8_t *gpu_out = (uint8_t *)malloc(max_bytes);
    if (!gpu_out) return 0;
    GeState saved = *s_ge;                 /* the lists leave the stencil test and masks on */
    int ok = 1;
    unsigned runs = 0, differing = 0;
    const unsigned long long splits0 = s_replay_stats.stencil_splits;
    for (uint32_t fmt = 0; fmt < 4u && ok; fmt++) {
        for (int depth = 0; depth < 2 && ok; depth++) {
            const uint32_t bytes = FB_W * FB_H * (fmt == 3u ? 4u : 2u);
            const uint32_t list = depth ? stp_depth_list(base, fmt) : stp_matrix_list(base, fmt);
            coherence_reset_targets();
            stp_fill_background(base, fmt);
            const unsigned long long fallback0 = s_replay_stats.fallback_prims;
            ge_set_gpu_hooks(&k_hooks);
            ge_run_list(list, 0);
            GeGpuFbDescriptor desc = { .addr = base, .format = fmt, .stride = 512u,
                                       .width = 480, .height = FB_H };
            if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) { ok = 0; break; }
            memcpy(gpu_out, SR_HOST(base), bytes);
            const unsigned long long fallbacks = s_replay_stats.fallback_prims - fallback0;

            coherence_reset_targets();
            stp_fill_background(base, fmt);
            ge_set_gpu_hooks(NULL);
            ge_run_list(list, 0);
            ge_set_gpu_hooks(&k_hooks);
            const uint8_t *sw_out = (const uint8_t *)SR_HOST(base);
            runs++;
            /* The list must draw: count what ge.c changed against a fresh background. */
            const uint32_t bpp = fmt == 3u ? 4u : 2u;
            unsigned drawn = 0;
            for (uint32_t i = 0; i < FB_W * FB_H; i++) {
                uint32_t bg = wrap_hash(0x3C1u + i * 40503u), w = 0;
                memcpy(&w, sw_out + i * bpp, bpp);
                if (fmt != 3u) bg &= 0xFFFFu;
                if (w != bg) drawn++;
            }
            if (drawn < 4096u) {
                fprintf(stderr, "gpu coherence selftest [stencil-parity]: fmt=%u %s list changed "
                                "only %u pixels\n", fmt, depth ? "depth" : "matrix", drawn);
                ok = 0;
            }
            if (fallbacks) {
                fprintf(stderr, "gpu coherence selftest [stencil-parity]: fmt=%u %s list took "
                                "the software fallback for %llu primitives\n",
                        fmt, depth ? "depth" : "matrix", fallbacks);
                ok = 0;
            }
            for (uint32_t i = 0; i < FB_W * FB_H; i++) {
                if (i % FB_W >= 480u || !memcmp(gpu_out + i * bpp, sw_out + i * bpp, bpp)) continue;
                if (!differing++) {
                    uint32_t g = 0, w = 0;
                    memcpy(&g, gpu_out + i * bpp, bpp); memcpy(&w, sw_out + i * bpp, bpp);
                    uint32_t x = i % FB_W, y = i / FB_W;
                    uint32_t cell = (y - STP_ORIGIN) / STP_PITCH * STP_CELLS_X + (x - STP_ORIGIN) / STP_PITCH;
                    fprintf(stderr, "gpu coherence selftest [stencil-parity]: fmt=%u %s cell=%u "
                                    "x=%u y=%u gpu=%08x ge_c=%08x\n",
                            fmt, depth ? "depth" : "matrix", cell, x, y, g, w);
                }
                ok = 0;
            }
            coherence_reset_targets();
        }
    }
    free(gpu_out);
    *s_ge = saved;
    const unsigned long long splits = s_replay_stats.stencil_splits - splits0;
    if (ok && !splits) {
        fprintf(stderr, "gpu coherence selftest [stencil-parity]: no state split its batch\n");
        ok = 0;
    }
    if (!ok) {
        fprintf(stderr, "gpu coherence selftest [stencil-parity]: %u pixels differ from ge.c\n",
                differing);
        return 0;
    }
    printf("gpu coherence selftest: PASS %-22s lists=%u splits=%llu max_delta_vs_ge_c=0\n",
           "stencil-parity", runs, splits);
    return 1;
}

/* The fallback classifier names the first unsupported state, in the order the support
 * check always tested them, and a supported state names none. */
static int coherence_run_fallback_reasons(void) {
    GeState saved = *s_ge;
    int ok = 1;
    struct { const char *what; int want; } cells[] = {
        {"stencil", GEGPU_FALLBACK_STENCIL}, {"color test", GEGPU_FALLBACK_COLOR_TEST},
        {"logic op", GEGPU_FALLBACK_LOGIC_OP}, {"ram fb", GEGPU_FALLBACK_RAM_FB},
        {"wide stride", GEGPU_FALLBACK_WIDE_STRIDE},
    };
    s_ge->stencil_enable = s_ge->color_test_enable = s_ge->logic_op_enable = 0;
    s_ge->clear = 0; s_ge->mask_rgb = 0;   /* an exact write mask */
    s_ge->ztest_enable = 1;                /* a depth-tested stencil-fail INCR: unsupported */
    s_ge->stencil_test = 2u; s_ge->stencil_op = 4u;
    s_ge->fbp = 0x04000000u; s_ge->fbw = 512u;
    s_ge->tex_enable = 0;
    if (gpu_state_unsupported_reason() != -1) ok = 0;
    for (unsigned i = 0; i < sizeof(cells) / sizeof(cells[0]); i++) {
        s_ge->stencil_enable = i == 0;
        s_ge->color_test_enable = i == 1;
        s_ge->logic_op_enable = i == 2;
        s_ge->fbp = i == 3 ? 0x08000000u : 0x04000000u;   /* main RAM, not VRAM */
        s_ge->fbw = i == 4 ? 1024u : 512u;
        const int got = gpu_state_unsupported_reason();
        if (got != cells[i].want) {
            fprintf(stderr, "gpu coherence selftest [fallback-reasons]: %s gave %d, want %d\n",
                    cells[i].what, got, cells[i].want);
            ok = 0;
        }
    }
    s_ge->fbp = 0x04000000u; s_ge->fbw = 512u;
    s_ge->stencil_enable = 1; s_ge->color_test_enable = 1;   /* stencil is tested first */
    if (gpu_state_unsupported_reason() != GEGPU_FALLBACK_STENCIL) ok = 0;
    /* The stencil states the GPU path takes: clear mode (ge.c ignores the test there), no
     * depth test, or KEEP for the ops the depth test can leave to run. */
    struct { const char *what; int clear, ztest; uint32_t test, op; int want; } st[] = {
        {"stencil clear", 1, 1, 2u, 0x000004u, -1},
        {"stencil no depth", 0, 0, 2u, 0x040404u, -1},
        {"stencil always", 0, 1, 1u, 0x020004u, -1},
        {"stencil keep", 0, 1, 2u, 0x050000u, -1},
        {"stencil zfail op", 0, 1, 1u, 0x000100u, GEGPU_FALLBACK_STENCIL},
    };
    s_ge->color_test_enable = 0;
    for (unsigned i = 0; i < sizeof(st) / sizeof(st[0]); i++) {
        s_ge->clear = st[i].clear; s_ge->ztest_enable = st[i].ztest;
        s_ge->stencil_test = st[i].test; s_ge->stencil_op = st[i].op;
        const int got = gpu_state_unsupported_reason();
        if (got != st[i].want) {
            fprintf(stderr, "gpu coherence selftest [fallback-reasons]: %s gave %d, want %d\n",
                    st[i].what, got, st[i].want);
            ok = 0;
        }
    }
    *s_ge = saved;
    if (ok) printf("gpu coherence selftest: PASS %-22s 11 cells\n", "fallback-reasons");
    return ok;
}

/* GPU transform and lighting parity (#318). Each cell is one display list of project-written
 * command words and generated vertices (no game data) drawing transform-mode triangles into a
 * cleared 512x272 8888 target with a depth buffer. It runs three ways: through the backend with
 * GPU T&L on, through the backend with it off (ge.c's T&L, then hook_tri), and in software.
 * The first two must leave byte-identical colour and depth, and the T&L run must count every
 * transform PRIM as drawn by psp_tnl.vert, or, for a cell whose state ge.c declines (a
 * near-plane crossing, specular light, texgen, flat shading), as declined: a run that silently
 * took the other route would prove nothing. The software run is reported, not asserted: the
 * Vulkan rasterizer's own tolerances against ge.c are the blend and sample-point fixtures'. On a
 * device without fp32 subnormal preservation there is no T&L stage, and every cell must take
 * ge.c's route and match: the fallback is what that device can show. */
enum {
    TNLC_STATIC, TNLC_STRIP_CULL0, TNLC_STRIP_CULL1, TNLC_FAN_IDX16, TNLC_LIST_IDX8,
    TNLC_TEXTURED, TNLC_FOG_LINEAR, TNLC_FOG_INF, TNLC_DEPTH, TNLC_RAMP, TNLC_FIXED_FMT,
    TNLC_MORPH2, TNLC_SKIN1, TNLC_SKIN2_U8, TNLC_SKIN4_U16, TNLC_SKIN8_F, TNLC_LIGHT_DIR,
    TNLC_LIGHT_POINT, TNLC_LIGHT4_MATUPD, TNLC_LIGHT_SKIN, TNLC_OFFSET_VIEWPORT,
    TNLC_NEAR_CROSS, TNLC_SPECULAR, TNLC_FLAT, TNLC_TEXGEN, TNLC_LIGHT_SUBNORMAL,
};

typedef struct TnlCell {
    const char *name;
    int kind;
    int gpu;          /* 1: drawn by psp_tnl.vert, 0: declined to ge.c */
} TnlCell;

#define TNLP_FBA     0x00000000u                 /* VRAM-relative colour buffer */
#define TNLP_ZBA     0x00100000u                 /* VRAM-relative depth buffer */
#define TNLP_TEX     0x04180000u                 /* 8x8 8888 texture */
#define TNLP_LIST    0x00200000u                 /* the harness arena, as the other fixtures */
#define TNLP_VTX     0x00210000u
#define TNLP_IDX     0x00280000u
#define TNLP_STRIDE  512u

static uint32_t tnlp_f24(float f) { uint32_t u; memcpy(&u, &f, 4); return u >> 8; }

/* Vertex writer following decode_vtype's order and alignment: weights, texture coordinate,
 * colour (8888 only), normal, position; each field aligned to its own size, the vertex to the
 * largest. fmt values are the vertex type's field codes (0 none, 1 8-bit, 2 16-bit, 3 float). */
typedef struct { int wfmt, wn, tc, col, nrm, pos, morph; } TnlpFmt;

static uint32_t tnlp_vtype(const TnlpFmt *f, int idx) {
    uint32_t vt = (uint32_t)f->tc | ((uint32_t)(f->col ? 7 : 0) << 2) | ((uint32_t)f->nrm << 5) |
                  ((uint32_t)f->pos << 7) | ((uint32_t)idx << 11);
    if (f->wfmt) vt |= ((uint32_t)f->wfmt << 9) | ((uint32_t)(f->wn - 1) << 14);
    if (f->morph > 1) vt |= (uint32_t)(f->morph - 1) << 18;
    return vt;
}

static uint32_t tnlp_align(uint32_t off, uint32_t a) { return (off + a - 1u) & ~(a - 1u); }

static void tnlp_put(uint8_t *d, uint32_t off, int fmt, float v, int is_unsigned) {
    if (fmt == 3) { memcpy(d + off, &v, 4); return; }
    if (fmt == 2) {
        int32_t q = (int32_t)lrintf(v * 32768.0f);
        if (is_unsigned) { if (q < 0) q = 0; if (q > 65535) q = 65535; }
        else { if (q < -32768) q = -32768; if (q > 32767) q = 32767; }
        uint16_t s = (uint16_t)q; memcpy(d + off, &s, 2); return;
    }
    int32_t q = (int32_t)lrintf(v * 128.0f);
    if (is_unsigned) { if (q < 0) q = 0; if (q > 255) q = 255; }
    else { if (q < -128) q = -128; if (q > 127) q = 127; }
    d[off] = (uint8_t)q;
}

/* One morph target of one vertex at d; returns the target's stride. */
static uint32_t tnlp_vertex(uint8_t *d, const TnlpFmt *f, const float *w, float u, float v,
                            uint32_t col, const float *n, const float *p) {
    static const uint32_t sz[4] = { 0, 1, 2, 4 };
    uint32_t off = 0, maxa = 1;
    if (f->wfmt) {
        uint32_t a = sz[f->wfmt];
        off = tnlp_align(off, a);
        for (int i = 0; i < f->wn; i++) tnlp_put(d, off + a * (uint32_t)i, f->wfmt, w[i], 1);
        off += a * (uint32_t)f->wn;
        if (a > maxa) maxa = a;
    }
    if (f->tc) {
        uint32_t a = sz[f->tc];
        off = tnlp_align(off, a);
        tnlp_put(d, off, f->tc, u, 1); tnlp_put(d, off + a, f->tc, v, 1);
        off += 2u * a;
        if (a > maxa) maxa = a;
    }
    if (f->col) {
        off = tnlp_align(off, 4u);
        memcpy(d + off, &col, 4);
        off += 4u;
        if (maxa < 4u) maxa = 4u;
    }
    if (f->nrm) {
        uint32_t a = sz[f->nrm];
        off = tnlp_align(off, a);
        for (int i = 0; i < 3; i++) tnlp_put(d, off + a * (uint32_t)i, f->nrm, n[i], 0);
        off += 3u * a;
        if (a > maxa) maxa = a;
    }
    uint32_t a = sz[f->pos];
    off = tnlp_align(off, a);
    for (int i = 0; i < 3; i++) tnlp_put(d, off + a * (uint32_t)i, f->pos, p[i], 0);
    off += 3u * a;
    if (a > maxa) maxa = a;
    return tnlp_align(off, maxa);
}

typedef struct { uint32_t *dl; int n; } TnlpList;
#define TNLP_W(L, cmd, data) ((L)->dl[(L)->n++] = ((uint32_t)(cmd) << 24) | ((uint32_t)(data) & 0x00FFFFFFu))

static void tnlp_matrix(TnlpList *L, uint32_t num_cmd, const float *m, int count) {
    TNLP_W(L, num_cmd, 0);
    for (int i = 0; i < count; i++) TNLP_W(L, num_cmd + 1u, tnlp_f24(m[i]));
}

/* A deterministic pseudo-random sequence: the same vertices on every run and host. */
static uint32_t s_tnlp_seed;
static float tnlp_rand(float lo, float hi) {
    s_tnlp_seed = s_tnlp_seed * 1664525u + 1013904223u;
    return lo + (hi - lo) * (float)(s_tnlp_seed >> 8) * (1.0f / 16777216.0f);
}

/* A rotation about the axis (ax, ay, az) by `ang` radians, translated by t, as a 4x3 matrix. */
static void tnlp_rot(float *m, float ax, float ay, float az, float ang, float tx, float ty, float tz) {
    float l = sqrtf(ax * ax + ay * ay + az * az);
    ax /= l; ay /= l; az /= l;
    float c = cosf(ang), s = sinf(ang), k = 1.0f - c;
    m[0] = c + ax * ax * k;      m[1] = ay * ax * k + az * s; m[2] = az * ax * k - ay * s;
    m[3] = ax * ay * k - az * s; m[4] = c + ay * ay * k;      m[5] = az * ay * k + ax * s;
    m[6] = ax * az * k + ay * s; m[7] = ay * az * k - ax * s; m[8] = c + az * az * k;
    m[9] = tx; m[10] = ty; m[11] = tz;
}

/* Build cell `c`'s list; returns the list address. */
static uint32_t tnlp_build(const TnlCell *c) {
    TnlpList L = { (uint32_t *)SR_HOST(TNLP_LIST), 0 };
    s_tnlp_seed = 0x1234567u + (uint32_t)c->kind * 7919u;
    const uint32_t fb = 0x04000000u | TNLP_FBA;
    TNLP_W(&L, 0x10, 0);
    TNLP_W(&L, 0x9C, fb & 0x00FFFFFFu);
    TNLP_W(&L, 0x9D, TNLP_STRIDE | ((fb & 0xFF000000u) >> 8));
    TNLP_W(&L, 0xD2, 3);
    TNLP_W(&L, 0x9E, TNLP_ZBA);
    TNLP_W(&L, 0x9F, TNLP_STRIDE);
    TNLP_W(&L, 0x15, 0);
    TNLP_W(&L, 0x16, ((FB_H - 1u) << 10) | (480u - 1u));
    TNLP_W(&L, 0xD4, 0);
    TNLP_W(&L, 0xD5, ((FB_H - 1u) << 10) | (480u - 1u));
    TNLP_W(&L, 0xD6, 0);
    TNLP_W(&L, 0xD7, 0xFFFF);
    for (uint32_t cmd = 0x17; cmd <= 0x28; cmd++) TNLP_W(&L, cmd, 0);   /* every unit off */
    TNLP_W(&L, 0xE7, 0);                  /* depth writes on */
    TNLP_W(&L, 0xDE, 1);                  /* depth test ALWAYS when enabled */
    TNLP_W(&L, 0x50, c->kind == TNLC_FLAT ? 0 : 1);
    TNLP_W(&L, 0xE8, 0);
    TNLP_W(&L, 0xE9, 0);

    /* Clear colour and depth with a through-mode clear sprite. */
    const uint32_t clear_vtx = TNLP_VTX + 0x60000u;
    {
        uint8_t *d = (uint8_t *)SR_HOST(clear_vtx);
        const uint32_t bg = 0xff302010u;
        const float cz = c->kind == TNLC_DEPTH ? 65535.0f : 0.0f;
        float p0[3] = { 0.0f, 0.0f, cz }, p1[3] = { 480.0f, 272.0f, cz };
        memcpy(d, &bg, 4); memcpy(d + 4, p0, 12);
        memcpy(d + 16, &bg, 4); memcpy(d + 20, p1, 12);
        TNLP_W(&L, 0xD3, 0x701);
        TNLP_W(&L, 0x12, (7u << 2) | (3u << 7) | (1u << 23));
        TNLP_W(&L, 0x10, (clear_vtx >> 8) & 0x000F0000u);
        TNLP_W(&L, 0x01, clear_vtx);
        TNLP_W(&L, 0x04, (6u << 16) | 2u);
        TNLP_W(&L, 0xD3, 0);
    }

    /* Matrices: a perspective camera looking at the origin from +z. */
    float world[12], view[12], proj[16];
    tnlp_rot(world, 0.3f, 1.0f, 0.2f, 0.6f, 0.0f, 0.0f, 0.0f);
    tnlp_rot(view, 1.0f, 0.0f, 0.0f, 0.15f, 0.05f, -0.1f, -6.0f);
    {
        const float fov = 1.0f, aspect = 480.0f / 272.0f, zn = 0.5f, zf = 100.0f;
        float f = 1.0f / tanf(fov * 0.5f);
        memset(proj, 0, sizeof(proj));
        proj[0] = f / aspect; proj[5] = f;
        proj[10] = (zf + zn) / (zn - zf); proj[11] = -1.0f;
        proj[14] = 2.0f * zf * zn / (zn - zf);
    }
    if (c->kind == TNLC_NEAR_CROSS) view[11] = -1.2f;   /* the mesh reaches through the near plane */
    if (c->kind == TNLC_LIGHT_SUBNORMAL) {
        /* World 3x3 scaled by 2^-30 and view 3x3 by 2^30: the same screen positions (powers of
         * two scale exactly), but world-space normals near 2^-66, whose squared length is
         * subnormal before light_vertex normalises them. */
        for (int i = 0; i < 9; i++) { world[i] = ldexpf(world[i], -30); view[i] = ldexpf(view[i], 30); }
    }
    tnlp_matrix(&L, 0x3A, world, 12);
    tnlp_matrix(&L, 0x3C, view, 12);
    tnlp_matrix(&L, 0x3E, proj, 16);
    const float vxs = c->kind == TNLC_OFFSET_VIEWPORT ? 230.0f : 240.0f;
    const float vys = c->kind == TNLC_OFFSET_VIEWPORT ? -130.0f : -136.0f;
    TNLP_W(&L, 0x42, tnlp_f24(vxs));
    TNLP_W(&L, 0x43, tnlp_f24(vys));
    TNLP_W(&L, 0x44, tnlp_f24(-32000.0f));
    TNLP_W(&L, 0x45, tnlp_f24(2048.0f));
    TNLP_W(&L, 0x46, tnlp_f24(2048.0f));
    TNLP_W(&L, 0x47, tnlp_f24(32767.5f));
    if (c->kind == TNLC_OFFSET_VIEWPORT) {
        TNLP_W(&L, 0x4C, ((2048u - 240u) << 4) + 5u);   /* a sub-pixel offset */
        TNLP_W(&L, 0x4D, ((2048u - 136u) << 4) + 11u);
    } else {
        TNLP_W(&L, 0x4C, (2048u - 240u) << 4);
        TNLP_W(&L, 0x4D, (2048u - 136u) << 4);
    }
    TNLP_W(&L, 0x48, tnlp_f24(1.0f));
    TNLP_W(&L, 0x49, tnlp_f24(1.0f));
    TNLP_W(&L, 0x4A, 0);
    TNLP_W(&L, 0x4B, 0);
    TNLP_W(&L, 0xC0, c->kind == TNLC_TEXGEN ? 1u : 0u);

    /* Texture for the textured cells. */
    if (c->kind == TNLC_TEXTURED || c->kind == TNLC_TEXGEN || c->kind == TNLC_FIXED_FMT) {
        uint32_t *tex = (uint32_t *)SR_HOST(TNLP_TEX);
        for (int v = 0; v < 8; v++)
            for (int u = 0; u < 8; u++)
                tex[v * 8 + u] = 0xff000000u | ((uint32_t)(v * 32) << 8) | (uint32_t)(u * 32) |
                                 (((u ^ v) & 1) ? 0x00c00000u : 0x00200000u);
        sr_gpu_vram_dirty(TNLP_TEX, 8u * 8u * 4u);
        TNLP_W(&L, 0x1E, 1);
        TNLP_W(&L, 0xA0, TNLP_TEX & 0x00FFFFFFu);
        TNLP_W(&L, 0xA8, 8u | ((TNLP_TEX & 0xFF000000u) >> 8));
        TNLP_W(&L, 0xB8, (3u << 8) | 3u);
        TNLP_W(&L, 0xC2, 0);
        TNLP_W(&L, 0xC3, 3);
        TNLP_W(&L, 0xC6, 0);
        TNLP_W(&L, 0xC7, 0);                   /* repeat */
        TNLP_W(&L, 0xC9, 0x100u | 0u);         /* MODULATE, RGBA */
        TNLP_W(&L, 0xCB, 0);
        if (c->kind == TNLC_TEXTURED) {
            TNLP_W(&L, 0x48, tnlp_f24(1.75f));
            TNLP_W(&L, 0x49, tnlp_f24(-0.5f));
            TNLP_W(&L, 0x4A, tnlp_f24(0.125f));
            TNLP_W(&L, 0x4B, tnlp_f24(0.3f));
        }
        if (c->kind == TNLC_TEXGEN) {
            float tg[12]; tnlp_rot(tg, 0.0f, 0.0f, 1.0f, 0.4f, 0.5f, 0.5f, 1.0f);
            tnlp_matrix(&L, 0x40, tg, 12);
            TNLP_W(&L, 0xC0, 1u | (0u << 8));
        }
    }

    if (c->kind == TNLC_FOG_LINEAR || c->kind == TNLC_FOG_INF) {
        TNLP_W(&L, 0x1F, 1);
        TNLP_W(&L, 0xCF, 0x00406080u);
        if (c->kind == TNLC_FOG_LINEAR) {
            TNLP_W(&L, 0xCD, tnlp_f24(7.5f));
            TNLP_W(&L, 0xCE, tnlp_f24(0.37f));
        } else {
            TNLP_W(&L, 0xCD, 0x7F8000u);           /* +infinity */
            TNLP_W(&L, 0xCE, tnlp_f24(1.0f));
        }
    }

    if (c->kind == TNLC_DEPTH || c->kind == TNLC_STATIC) {
        TNLP_W(&L, 0x23, 1);
        TNLP_W(&L, 0xDE, c->kind == TNLC_DEPTH ? 4u : 6u);   /* LESS / GREATER */
    }
    if (c->kind == TNLC_STRIP_CULL0 || c->kind == TNLC_STRIP_CULL1) {
        TNLP_W(&L, 0x1D, 1);
        TNLP_W(&L, 0x9B, c->kind == TNLC_STRIP_CULL1 ? 1u : 0u);
    }

    /* Bones: each a rotation with a translation. */
    int skin_w = c->kind == TNLC_SKIN1 ? 1 : c->kind == TNLC_SKIN2_U8 ? 2 :
                 c->kind == TNLC_SKIN4_U16 ? 4 : c->kind == TNLC_SKIN8_F ? 8 :
                 c->kind == TNLC_LIGHT_SKIN ? 3 : 0;
    if (skin_w) {
        TNLP_W(&L, 0x2A, 0);
        for (int b = 0; b < 8; b++) {
            float m[12];
            tnlp_rot(m, tnlp_rand(-1, 1), tnlp_rand(-1, 1), tnlp_rand(0.2f, 1), tnlp_rand(-0.5f, 0.5f),
                     tnlp_rand(-0.3f, 0.3f), tnlp_rand(-0.3f, 0.3f), tnlp_rand(-0.3f, 0.3f));
            for (int i = 0; i < 12; i++) TNLP_W(&L, 0x2B, tnlp_f24(m[i]));
        }
    }
    if (c->kind == TNLC_MORPH2) {
        TNLP_W(&L, 0x2C, tnlp_f24(0.625f));
        TNLP_W(&L, 0x2D, tnlp_f24(0.375f));
    }

    /* Lighting. */
    int lit = c->kind == TNLC_LIGHT_DIR || c->kind == TNLC_LIGHT_POINT ||
              c->kind == TNLC_LIGHT4_MATUPD || c->kind == TNLC_LIGHT_SKIN || c->kind == TNLC_SPECULAR ||
              c->kind == TNLC_LIGHT_SUBNORMAL;
    if (lit) {
        TNLP_W(&L, 0x17, 1);
        TNLP_W(&L, 0x53, c->kind == TNLC_LIGHT4_MATUPD ? 3u : 0u);
        TNLP_W(&L, 0x54, 0x00080402u);
        TNLP_W(&L, 0x55, 0x00405060u);
        TNLP_W(&L, 0x56, 0x00c0a0e0u);
        TNLP_W(&L, 0x57, 0x00ffffffu);
        TNLP_W(&L, 0x58, 0xb0);
        /* A positive power only where the cell means to decline (powf); the powered-diffuse
           light of the four-light cell then lights as plain diffuse. */
        TNLP_W(&L, 0x5B, tnlp_f24(c->kind == TNLC_SPECULAR ? 8.0f : 0.0f));
        TNLP_W(&L, 0x5C, 0x00303030u);
        TNLP_W(&L, 0x5D, 0xc0);
        int nl = c->kind == TNLC_LIGHT4_MATUPD ? 4 : 1;
        for (int i = 0; i < nl; i++) {
            int point = c->kind == TNLC_LIGHT_POINT || (c->kind == TNLC_LIGHT4_MATUPD && (i & 1));
            uint32_t comp = c->kind == TNLC_SPECULAR ? 1u : (c->kind == TNLC_LIGHT4_MATUPD && i == 2 ? 2u : 0u);
            TNLP_W(&L, 0x18 + (uint32_t)i, 1);
            TNLP_W(&L, 0x5F + (uint32_t)i, comp | ((uint32_t)(point ? 1 : 0) << 8));
            float pos[3] = { tnlp_rand(-3, 3), tnlp_rand(-3, 3), tnlp_rand(1, 4) };
            for (int k = 0; k < 3; k++) TNLP_W(&L, 0x63 + (uint32_t)(3 * i + k), tnlp_f24(pos[k]));
            float att[3] = { 0.6f, 0.15f, 0.02f };
            for (int k = 0; k < 3; k++) TNLP_W(&L, 0x7B + (uint32_t)(3 * i + k), tnlp_f24(att[k]));
            TNLP_W(&L, 0x8F + (uint32_t)(3 * i), 0x00201810u + (uint32_t)i * 0x00080402u);
            TNLP_W(&L, 0x90 + (uint32_t)(3 * i), 0x00c0d0e0u - (uint32_t)i * 0x00102030u);
            TNLP_W(&L, 0x91 + (uint32_t)(3 * i), 0x00ffffffu);
        }
    }

    /* The mesh: a grid of 12 x 8 quads over [-1.6, 1.6] x [-1.1, 1.1], displaced in z, with
     * colours, normals, texture coordinates and weights from the sequence. */
    TnlpFmt f = { 0, 0, 3, 1, 3, 3, 1 };
    if (skin_w) {
        f.wfmt = c->kind == TNLC_SKIN2_U8 ? 1 : c->kind == TNLC_SKIN4_U16 ? 2 : 3;
        f.wn = skin_w;
    }
    if (c->kind == TNLC_FIXED_FMT) { f.tc = 2; f.nrm = 1; f.pos = 2; }
    if (c->kind == TNLC_MORPH2) f.morph = 2;
    const int gw = 12, gh = 8;
    uint8_t *vd = (uint8_t *)SR_HOST(TNLP_VTX);
    uint32_t stride = 0, nverts = 0;
    for (int y = 0; y <= gh; y++) {
        for (int x = 0; x <= gw; x++) {
            float scale = c->kind == TNLC_FIXED_FMT ? 0.5f : 1.0f;
            float p[3] = { (-1.6f + 3.2f * (float)x / (float)gw) * scale,
                           (-1.1f + 2.2f * (float)y / (float)gh) * scale,
                           tnlp_rand(-0.3f, 0.3f) * scale };
            float n[3] = { tnlp_rand(-0.5f, 0.5f), tnlp_rand(-0.5f, 0.5f), 0.8f };
            if (c->kind == TNLC_LIGHT_SUBNORMAL)
                for (int i = 0; i < 3; i++)   /* every component at least 2^-40, as ge.c requires */
                    n[i] = ldexpf(n[i] < 0.0f ? n[i] - 0.1f : n[i] + 0.1f, -35);
            float w[8];
            float sum = 0.0f;
            for (int i = 0; i < 8; i++) { w[i] = i < skin_w ? tnlp_rand(0.0f, 1.0f) : 0.0f; sum += w[i]; }
            if (skin_w) for (int i = 0; i < skin_w; i++) w[i] = w[i] / sum;
            if (skin_w > 1) w[0] = 0.0f;   /* a zero weight: skipped */
            uint32_t col = 0xff000000u | ((uint32_t)(tnlp_rand(0, 255)) << 16) |
                           ((uint32_t)(tnlp_rand(0, 255)) << 8) | (uint32_t)(tnlp_rand(0, 255));
            if (c->kind == TNLC_RAMP) {
                uint32_t k = (uint32_t)(y * (gw + 1) + x) * 2u;
                col = 0xff000000u | ((k & 255u) << 16) | (((k * 3u) & 255u) << 8) | ((255u - k) & 255u);
            }
            float u = (float)x / (float)gw, v = (float)y / (float)gh;
            uint8_t *at = vd + (size_t)nverts * (stride ? stride : 0);
            stride = 0;
            for (int m = 0; m < f.morph; m++) {
                float pm[3] = { p[0] + 0.2f * (float)m, p[1] - 0.1f * (float)m, p[2] };
                stride += tnlp_vertex(at + stride, &f, w, u, v, col, n, pm);
            }
            nverts++;
        }
    }
    uint32_t vt_idx = 0;
    uint32_t prim = 0, count = 0;
    TNLP_W(&L, 0x10, (TNLP_VTX >> 8) & 0x000F0000u);
    if (c->kind == TNLC_FAN_IDX16 || c->kind == TNLC_LIST_IDX8) {
        /* An index buffer over the grid's vertices. */
        uint8_t *ib = (uint8_t *)SR_HOST(TNLP_IDX);
        uint32_t ni = 0;
        if (c->kind == TNLC_FAN_IDX16) {
            /* A fan around the grid's centre through a ring of vertices. */
            uint16_t centre = (uint16_t)((gh / 2) * (gw + 1) + gw / 2);
            memcpy(ib + 2u * ni++, &centre, 2);
            for (int k = 0; k <= 16; k++) {
                float a = 6.2831853f * (float)k / 16.0f;
                int gx = gw / 2 + (int)lrintf(5.0f * cosf(a)), gy = gh / 2 + (int)lrintf(3.0f * sinf(a));
                uint16_t ix = (uint16_t)(gy * (gw + 1) + gx);
                memcpy(ib + 2u * ni++, &ix, 2);
            }
            vt_idx = 2; prim = 5;
        } else {
            for (int y = 0; y < gh && ni + 6u <= 255u * 3u; y += 2) {
                for (int x = 0; x < gw; x += 2) {
                    uint8_t a = (uint8_t)(y * (gw + 1) + x), b = (uint8_t)(a + 1);
                    uint8_t c2 = (uint8_t)(a + gw + 1), d2 = (uint8_t)(c2 + 1);
                    ib[ni++] = a; ib[ni++] = b; ib[ni++] = c2;
                    ib[ni++] = b; ib[ni++] = d2; ib[ni++] = c2;
                }
            }
            vt_idx = 1; prim = 3;
        }
        count = ni;
        TNLP_W(&L, 0x12, tnlp_vtype(&f, (int)vt_idx));
        TNLP_W(&L, 0x02, TNLP_IDX & 0x00FFFFFFu);
        TNLP_W(&L, 0x01, TNLP_VTX & 0x00FFFFFFu);
        TNLP_W(&L, 0x04, (prim << 16) | count);
    } else if (c->kind == TNLC_STRIP_CULL0 || c->kind == TNLC_STRIP_CULL1 || c->kind == TNLC_FLAT ||
               c->kind == TNLC_MORPH2 || c->kind == TNLC_FIXED_FMT) {
        /* One strip per grid row pair, through indices: vertices (y, x), (y + 1, x), ... */
        uint8_t *ib = (uint8_t *)SR_HOST(TNLP_IDX);
        TNLP_W(&L, 0x12, tnlp_vtype(&f, 2));
        TNLP_W(&L, 0x01, TNLP_VTX & 0x00FFFFFFu);
        for (int y = 0; y < gh; y++) {
            uint32_t at = TNLP_IDX + (uint32_t)y * 0x100u;
            uint16_t *row = (uint16_t *)(ib + (size_t)y * 0x100u);
            for (int x = 0; x <= gw; x++) {
                row[2 * x] = (uint16_t)(y * (gw + 1) + x);
                row[2 * x + 1] = (uint16_t)((y + 1) * (gw + 1) + x);
            }
            TNLP_W(&L, 0x02, at & 0x00FFFFFFu);
            TNLP_W(&L, 0x04, (4u << 16) | (uint32_t)(2 * (gw + 1)));
        }
    } else {
        /* A triangle list over the grid, two triangles per quad, written out vertex by vertex. */
        uint8_t *ld = (uint8_t *)SR_HOST(TNLP_VTX + 0x20000u);
        uint32_t nl = 0;
        for (int y = 0; y < gh; y++) {
            for (int x = 0; x < gw; x++) {
                int q[6] = { y * (gw + 1) + x, y * (gw + 1) + x + 1, (y + 1) * (gw + 1) + x,
                             y * (gw + 1) + x + 1, (y + 1) * (gw + 1) + x + 1, (y + 1) * (gw + 1) + x };
                for (int k = 0; k < 6; k++)
                    memcpy(ld + (size_t)nl++ * stride, vd + (size_t)q[k] * stride, stride);
            }
        }
        TNLP_W(&L, 0x12, tnlp_vtype(&f, 0));
        TNLP_W(&L, 0x10, ((TNLP_VTX + 0x20000u) >> 8) & 0x000F0000u);
        TNLP_W(&L, 0x01, (TNLP_VTX + 0x20000u) & 0x00FFFFFFu);
        TNLP_W(&L, 0x04, (3u << 16) | nl);
    }
    TNLP_W(&L, 0x0F, 0);
    TNLP_W(&L, 0x0C, 0);
    return TNLP_LIST;
}

typedef struct { uint32_t *color; uint16_t *depth; } TnlpOut;

/* Run cell c's list one way (mode 0: software, 1: backend with ge.c's T&L, 2: backend with
 * GPU T&L) and copy the colour and depth buffers out. */
static int tnlp_run(const TnlCell *c, int mode, TnlpOut *out, unsigned long long *taken) {
    const uint32_t fb = 0x04000000u | TNLP_FBA, zb = 0x04000000u | TNLP_ZBA;
    coherence_reset_targets();
    memset(SR_HOST(fb), 0, TNLP_STRIDE * FB_H * 4u);
    memset(SR_HOST(zb), 0, TNLP_STRIDE * FB_H * 2u);
    sr_gpu_vram_dirty(fb, TNLP_STRIDE * FB_H * 4u);
    sr_gpu_vram_dirty(zb, TNLP_STRIDE * FB_H * 2u);
    uint32_t list = tnlp_build(c);
    ge_tnl_set_enabled(mode == 2);
    ge_tnl_counts(NULL, NULL, 1);
    ge_set_gpu_hooks(mode ? &k_hooks : NULL);
    ge_run_list(list, 0);
    ge_set_gpu_hooks(&k_hooks);
    ge_tnl_counts(taken, NULL, 0);
    ge_tnl_set_enabled(-1);
    if (mode) {
        GeGpuFbDescriptor desc = { .addr = fb, .format = 3, .stride = TNLP_STRIDE, .width = 480, .height = FB_H };
        if (gegpu_sync_guest_fb(&desc) != GEGPU_SYNC_OK) return 0;
        if (s_nbatch && !submit_pending()) return 0;
        /* Only this fixture's depth surface: ge.c has one depth mirror, and reading an earlier
         * fixture's surface back into it would overwrite this one's. */
        for (int i = 0; i < MAX_DEP; i++)
            if (s_deps[i].used && s_deps[i].zba == TNLP_ZBA && !depth_to_cpu(&s_deps[i])) return 0;
    }
    memcpy(out->color, SR_HOST(fb), TNLP_STRIDE * FB_H * 4u);
    /* ge.c's depth surface: the software GE's own, or the backend's depth image read back. */
    const uint16_t *z = ge_zbuf_ptr();
    for (uint32_t y = 0; y < FB_H; y++)
        memcpy(out->depth + (size_t)y * TNLP_STRIDE, z + (size_t)y * ge_zbuf_stride(), TNLP_STRIDE * 2u);
    return 1;
}

static int coherence_run_tnl_parity(void) {
    static const TnlCell cells[] = {
        { "static-list-ztest", TNLC_STATIC, 1 },
        { "strip-cull0", TNLC_STRIP_CULL0, 1 },
        { "strip-cull1", TNLC_STRIP_CULL1, 1 },
        { "fan-index16", TNLC_FAN_IDX16, 1 },
        { "list-index8", TNLC_LIST_IDX8, 1 },
        { "textured-scale-offset", TNLC_TEXTURED, 1 },
        { "fog-linear", TNLC_FOG_LINEAR, 1 },
        { "fog-infinite-end", TNLC_FOG_INF, 1 },
        { "depth-less", TNLC_DEPTH, 1 },
        { "colour-ramp", TNLC_RAMP, 1 },
        { "fixed-formats", TNLC_FIXED_FMT, 1 },
        { "morph-2", TNLC_MORPH2, 1 },
        { "skin-1-float", TNLC_SKIN1, 1 },
        { "skin-2-u8", TNLC_SKIN2_U8, 1 },
        { "skin-4-u16", TNLC_SKIN4_U16, 1 },
        { "skin-8-float", TNLC_SKIN8_F, 1 },
        { "light-directional", TNLC_LIGHT_DIR, 1 },
        { "light-point", TNLC_LIGHT_POINT, 1 },
        { "light-4-matupdate", TNLC_LIGHT4_MATUPD, 1 },
        { "light-skinned", TNLC_LIGHT_SKIN, 1 },
        { "offset-viewport", TNLC_OFFSET_VIEWPORT, 1 },
        { "near-crossing", TNLC_NEAR_CROSS, 0 },
        { "specular", TNLC_SPECULAR, 0 },
        { "flat", TNLC_FLAT, 0 },
        { "texgen", TNLC_TEXGEN, 0 },
        { "light-subnormal-length", TNLC_LIGHT_SUBNORMAL, 1 },
    };
    const size_t px = (size_t)TNLP_STRIDE * FB_H;
    TnlpOut sw = { (uint32_t *)malloc(px * 4u), (uint16_t *)malloc(px * 2u) };
    TnlpOut cpu = { (uint32_t *)malloc(px * 4u), (uint16_t *)malloc(px * 2u) };
    TnlpOut gpu = { (uint32_t *)malloc(px * 4u), (uint16_t *)malloc(px * 2u) };
    int ok = sw.color && sw.depth && cpu.color && cpu.depth && gpu.color && gpu.depth;
    unsigned taken_cells = 0;
    const int buffers_ok = ok;
    for (size_t k = 0; buffers_ok && k < sizeof(cells) / sizeof(cells[0]); k++) {
        const TnlCell *c = &cells[k];
        unsigned long long t_sw = 0, t_cpu = 0, t_gpu = 0;
        if (!tnlp_run(c, 0, &sw, &t_sw) || !tnlp_run(c, 1, &cpu, &t_cpu) ||
            !tnlp_run(c, 2, &gpu, &t_gpu)) {
            fprintf(stderr, "gpu coherence selftest [tnl-parity/%s]: a run failed\n", c->name);
            ok = 0;
            break;
        }
        size_t cdiff = 0, zdiff = 0, first = px, zfirst = px;
        for (size_t i = 0; i < px; i++) {
            if (cpu.color[i] != gpu.color[i]) { cdiff++; if (first == px) first = i; }
            if (cpu.depth[i] != gpu.depth[i]) { zdiff++; if (zfirst == px) zfirst = i; }
        }
        size_t swdiff = 0, drawn = 0;
        int swmax = 0;
        for (size_t i = 0; i < px; i++) {
            if (cpu.color[i] != 0xff302010u) drawn++;
            if (sw.color[i] == cpu.color[i]) continue;
            swdiff++;
            for (int ch = 0; ch < 32; ch += 8) {
                int d = abs((int)((sw.color[i] >> ch) & 255u) - (int)((cpu.color[i] >> ch) & 255u));
                if (d > swmax) swmax = d;
            }
        }
        const int want_gpu = c->gpu && s_vs_tnl != VK_NULL_HANDLE;
        const int route_ok = want_gpu ? t_gpu > 0 : t_gpu == 0;
        if (cdiff || zdiff || !route_ok || !drawn) {
            fprintf(stderr, "gpu coherence selftest [tnl-parity/%s]: FAIL colour_diff=%zu depth_diff=%zu "
                    "gpu_tnl_prims=%llu (want %s) drawn=%zu",
                    c->name, cdiff, zdiff, t_gpu, want_gpu ? ">0" : "0", drawn);
            if (first < px)
                fprintf(stderr, " first=(%zu,%zu) cpu=%08x gpu=%08x", first % TNLP_STRIDE,
                        first / TNLP_STRIDE, (unsigned)cpu.color[first], (unsigned)gpu.color[first]);
            if (zfirst < px)
                fprintf(stderr, " zfirst=(%zu,%zu) cpu=%04x gpu=%04x sw=%04x", zfirst % TNLP_STRIDE,
                        zfirst / TNLP_STRIDE, cpu.depth[zfirst], gpu.depth[zfirst], sw.depth[zfirst]);
            fprintf(stderr, "\n");
            ok = 0;
            continue;
        }
        if (want_gpu) taken_cells++;
        printf("gpu coherence selftest: PASS tnl-parity %-22s gpu_tnl_prims=%llu drawn=%zu "
               "software_diff=%zu max_delta=%d\n", c->name, t_gpu, drawn, swdiff, swmax);
    }
    free(sw.color); free(sw.depth); free(cpu.color); free(cpu.depth); free(gpu.color); free(gpu.depth);
    if (ok)
        printf("gpu coherence selftest: PASS %-22s cells=%zu gpu_tnl_cells=%u%s\n", "tnl-parity",
               sizeof(cells) / sizeof(cells[0]), taken_cells,
               s_vs_tnl ? "" : " (no fp32 subnormal preservation: every cell checked on ge.c's route)");
    return ok;
}

/* The whole device-size image, not only the top-left device pixel of each scale x scale cell
 * that a readback to guest VRAM keeps (write_guest_fb). Colour is left in TRANSFER_SRC, as
 * target_readback leaves it; `after` is the layout the image returns to. */
static int coherence_read_device(VkImage img, VkImageAspectFlags aspect, VkImageLayout *layout,
                                 VkImageLayout after, uint32_t bpp, void *out) {
    if (s_nbatch && !submit_pending()) return 0;
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, img, aspect, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy c = {0};
    c.imageSubresource.aspectMask = aspect;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = SCL_W; c.imageExtent.height = SCL_H; c.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_xfer, 1, &c);
    to_layout(s_cmd, img, aspect, layout, after);
    if (!cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION)) return 0;
    memcpy(out, s_xfer_map, (size_t)SCL_W * SCL_H * bpp);
    return 1;
}

static int coherence_check_device_color(const char *step, const uint32_t *dev, const uint8_t *guest,
                                        uint32_t stride, uint32_t fmt) {
    const uint32_t bpp = fmt == 3u ? 4u : 2u;
    for (uint32_t y = 0; y < SCL_H; y++) {
        for (uint32_t x = 0; x < SCL_W; x++) {
            const uint32_t gx = x / (uint32_t)s_scale, gy = y / (uint32_t)s_scale;
            uint32_t raw = 0, want = 0;
            if (gx < stride) {                     /* columns past the stride upload as 0 */
                memcpy(&raw, guest + (gy * stride + gx) * bpp, bpp);
                want = fb_unpack(raw, fmt);
            }
            if (dev[y * SCL_W + x] != want) {
                fprintf(stderr, "gpu coherence selftest [scaled-upload]: %s fmt=%u stride=%u scale=%d: "
                        "device pixel (%u,%u) = 0x%08x, want 0x%08x\n", step, fmt, stride, s_scale,
                        x, y, dev[y * SCL_W + x], want);
                return 0;
            }
        }
    }
    return 1;
}

/* Every device pixel of each scale x scale cell holds its guest pixel after a whole-target
 * upload, after a CPU-write patch across a row boundary, and after a depth upload (#1296):
 * a readback to guest VRAM keeps only one device pixel per cell, so a row or column of a
 * cell left unwritten would not show there. Every format runs at a full and at a short
 * stride, whose columns past the stride upload as 0. At scale 1 the cells are single pixels;
 * gpu-coherence-selftest also runs this case alone at render scales 2 and 4. */
static int coherence_run_scaled_upload(void) {
    static const struct { uint32_t fmt, stride; } cells[] = {
        { 3u, 512u }, { 0u, 512u }, { 1u, 512u }, { 2u, 512u },
        { 3u, 480u }, { 0u, 480u }, { 1u, 448u }, { 2u, 496u },
    };
    const uint32_t fba = 0x00100000u, base = 0x04000000u | fba;
    uint32_t *dev = (uint32_t *)malloc((size_t)SCL_W * SCL_H * 4u);
    if (!dev) return 0;
    int ok = 1;
    for (uint32_t c = 0; ok && c < sizeof(cells) / sizeof(cells[0]); c++) {
        const uint32_t fmt = cells[c].fmt, stride = cells[c].stride, bpp = fmt == 3u ? 4u : 2u;
        coherence_reset_targets();
        uint8_t *guest = (uint8_t *)SR_HOST(base);
        for (uint32_t i = 0; i < stride * FB_H; i++) {
            const uint32_t raw = (i + 1u) * 0x9e3779b1u ^ (fmt << 24);
            memcpy(guest + i * bpp, &raw, bpp);
        }
        const char *fail = NULL;           /* "" once the check has printed its own line */
        Target *t = target_color_acquire(fba, stride, fmt, 1);
        if (!t || !t->gpu_valid)
            fail = "the target was not established";
        else if (!coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev))
            fail = "device read after the upload failed";
        else if (!coherence_check_device_color("whole-target upload", dev, guest, stride, fmt))
            fail = "";

        /* A CPU write from mid row 60 into row 61, held and applied as a patch. */
        if (!fail) {
            const uint32_t off = (60u * stride + 37u) * bpp, len = (stride + 50u) * bpp;
            for (uint32_t i = 0; i < len; i++) guest[off + i] = (uint8_t)(0x5au + i * 13u);
            const unsigned long long syncs0 = s_replay_stats.cpu_write_syncs;
            const unsigned long uploads0 = s_cnt_upload;
            sr_gpu_vram_dirty(base + off, len);
            const int held = t->cpu_n;
            t = target_color_acquire(fba, stride, fmt, 1);
            if (!t || !t->gpu_valid)
                fail = "the target was lost after the CPU write";
            else if (!held || s_replay_stats.cpu_write_syncs != syncs0 + 1u ||
                     s_cnt_upload != uploads0 + 1u) {   /* one held span, one patch */
                fprintf(stderr, "gpu coherence selftest [scaled-upload]: held=%d syncs=%llu uploads=%lu\n",
                        held, s_replay_stats.cpu_write_syncs - syncs0, s_cnt_upload - uploads0);
                fail = "the CPU write was not applied as a patch";
            }
            else if (!coherence_read_device(t->img, VK_IMAGE_ASPECT_COLOR_BIT, &t->layout,
                                            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 4u, dev))
                fail = "device read after the patch failed";
            else if (!coherence_check_device_color("CPU-write patch", dev, guest, stride, fmt))
                fail = "";
        }
        if (fail) {
            if (*fail)
                fprintf(stderr, "gpu coherence selftest [scaled-upload]: fmt=%u stride=%u scale=%d: %s\n",
                        fmt, stride, s_scale, fail);
            ok = 0;
        }
    }

    /* Depth: ge.c's CPU surface, uploaded at device size. */
    if (ok) {
        coherence_reset_targets();
        memset(s_ge, 0, sizeof(*s_ge));
        s_ge->fbp = base; s_ge->fbw = 512u;
        s_ge->zbp = 0x04080000u; s_ge->zbw = 512u;
        DepthEnt *d = NULL;
        uint16_t *dev16 = (uint16_t *)dev;
        ok = ge_zbuf_prepare_current() && (s_zbuf = ge_zbuf_ptr()) != NULL;
        if (ok) {
            for (uint32_t y = 0; y < FB_H; y++)
                for (uint32_t x = 0; x < FB_W; x++)
                    s_zbuf[y * 512u + x] = (uint16_t)(0x1234u + y * 977u + x * 31u);
            ge_zbuf_note_external_write();
            d = depth_acquire(0x00080000u, 512u);
        }
        if (d) d->cpu_dirty = 1;
        ok = d && depth_from_cpu(d) &&
             coherence_read_device(d->img, VK_IMAGE_ASPECT_DEPTH_BIT, &d->layout,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 2u, dev16);
        for (uint32_t y = 0; ok && y < SCL_H; y++) {
            for (uint32_t x = 0; x < SCL_W; x++) {
                const uint16_t want = s_zbuf[(y / (uint32_t)s_scale) * 512u + x / (uint32_t)s_scale];
                if (dev16[y * SCL_W + x] != want) {
                    fprintf(stderr, "gpu coherence selftest [scaled-upload]: depth upload scale=%d: "
                            "device pixel (%u,%u) = 0x%04x, want 0x%04x\n", s_scale, x, y,
                            dev16[y * SCL_W + x], want);
                    ok = 0;
                    break;
                }
            }
        }
        if (!ok && !d) fprintf(stderr, "gpu coherence selftest [scaled-upload]: no depth surface\n");
    }
    free(dev);
    coherence_reset_targets();
    if (ok)
        printf("gpu coherence selftest: PASS %-22s cells=%u scale=%d\n", "scaled-upload",
               (unsigned)(sizeof(cells) / sizeof(cells[0])), s_scale);
    return ok;
}

int gegpu_coherence_selftest(void) {
    /* SR_GPU_COHERENCE_CASE=scaled-upload (the harness's --case) runs that case alone, as the
     * recipe does at render scales 2 and 4: the other fixtures expect 1x pixels (a filtered sample
     * lands on other device pixels at a higher scale). Any other value fails. */
    const char *only = getenv("SR_GPU_COHERENCE_CASE");
    if (only && *only) {
        if (strcmp(only, "scaled-upload") == 0) return coherence_run_scaled_upload();
        if (strcmp(only, "mip-lod") == 0) return coherence_run_mip_lod();
        fprintf(stderr, "gpu coherence selftest: unknown SR_GPU_COHERENCE_CASE '%s'\n", only);
        return 0;
    }
    static const CoherenceCase cases[] = {
        { "8888-middle", 3, 512, (91u * 512u + 137u) * 4u, 4, 0, 0, 0 },
        { "5650-unaligned", 0, 512, (42u * 512u + 61u) * 2u + 1u, 3, 0, 0, 0 },
        { "5551-unaligned", 1, 512, (43u * 512u + 62u) * 2u + 1u, 3, 0, 0, 0 },
        { "4444-unaligned", 2, 512, (44u * 512u + 63u) * 2u + 1u, 3, 0, 0, 0 },
        { "8888-unaligned", 3, 512, (45u * 512u + 64u) * 4u + 1u, 5, 0, 0, 0 },
        { "row-start", 3, 512, 10u * 512u * 4u, 4, 0, 0, 0 },
        { "row-end", 3, 512, (11u * 512u + 511u) * 4u, 4, 0, 0, 0 },
        { "row-crossing", 3, 512, (12u * 512u + 511u) * 4u + 2u, 6, 0, 0, 0 },
        { "multi-row", 3, 512, (20u * 512u + 500u) * 4u, (12u + 512u + 17u) * 4u, 0, 0, 0 },
        { "stride-500", 3, 500, (30u * 500u + 499u) * 4u + 1u, 11, 0, 0, 0 },
        { "pending-batch", 3, 512, (70u * 512u + 90u) * 4u, 8, 1, 0, 0 },
        { "pending-async", 3, 512, (71u * 512u + 91u) * 4u, 8, 0, 1, 0 },
        { "alias-vram", 3, 512, (72u * 512u + 92u) * 4u, 8, 0, 0, 1 },
    };
    int ok = 1;
    for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++)
        if (!coherence_run_case(&cases[i])) ok = 0;
    if (!coherence_run_clean_case("clean-aligned", 3, 512, (50u * 512u + 20u) * 4u, 16)) ok = 0;
    if (!coherence_run_clean_case("clean-unaligned", 3, 512, (51u * 512u + 21u) * 4u + 1u, 5)) ok = 0;
    if (!coherence_run_full_cover_case()) ok = 0;
    if (!coherence_run_cpu_write_coalesce()) ok = 0;
    if (!coherence_run_overlap_case()) ok = 0;
    if (!coherence_run_tflt_descriptor_capacity()) ok = 0;
    if (!coherence_run_sprite_semantics()) ok = 0;
    if (!coherence_run_software_sprite_semantics()) ok = 0;
    if (!coherence_run_linear_sprite()) ok = 0;
    if (!coherence_run_tflt_filters()) ok = 0;
    if (!coherence_run_ring_wrap()) ok = 0;
    if (!coherence_run_submit_batch()) ok = 0;
    if (!coherence_run_upload_sequence()) ok = 0;
    if (!coherence_run_texture_dirty_same_state()) ok = 0;
    if (!coherence_run_texture_clean_page_index()) ok = 0;
    if (!coherence_run_texture_ram_unnotified_change()) ok = 0;
    if (!coherence_run_clut_format_same_palette()) ok = 0;
    if (!coherence_run_clut_format_list()) ok = 0;
    if (!coherence_run_blend_parity()) ok = 0;
    if (!coherence_run_scalar_vram_read(0x04080000u)) ok = 0;
    /* zbp 0x0a2000: bit 13 set, so the absolute XOR differs from the within-buffer form (0x0a6854 vs
     * 0x0a2854 for pixel 10,10). The 0x0ce000 placement used by the software selftest would overlap the
     * colour surface at 0x100000 here. */
    if (!coherence_run_scalar_vram_read(0x040A2000u)) ok = 0;
    if (!coherence_run_native_bulk_copy()) ok = 0;
    if (!coherence_run_fallback_reasons()) ok = 0;
    /* Last: its chain-fallback case draws through the software fallback, which hands the
     * current depth surface to ge.c, and the fixtures above start from the depth state the
     * ones before them leave. */
    if (!coherence_run_mip_levels()) ok = 0;
    if (!coherence_run_psp_sample_points()) ok = 0;
    /* After the mip fixtures: it rewrites the depth buffer they start from. */
    if (!coherence_run_stencil_parity()) ok = 0;
    if (!coherence_run_tnl_parity()) ok = 0;
    /* Last: it rewrites ge.c's depth surface. */
    if (!coherence_run_scaled_upload()) ok = 0;
    coherence_reset_targets();
    return ok;
}

/* Source-owned regression for the explicit snapshot boundary. It first proves that
 * ordinary presentation remains asynchronous (the pre-fix lag), then asks the same
 * target through gegpu_sync_guest_fb() and verifies that guest VRAM is current before
 * publication. Descriptor rejection and no-target answers are checked too. */
/* Ace Mode's texture filtering (ace.h), run with SR_ACE asking for 16x on a device that has it
 * (gpu-ace-selftest). Through-mode draws keep the guest's sampling exactly: the PSP-measured
 * sample points still hold and no anisotropic sampler is made for them. A transformed draw that
 * minifies linearly gets one anisotropic sampler, made once and reused; a nearest-filtered
 * texture, and the same entry drawn in through mode, keep the plain set. */
int gegpu_ace_af_selftest(void) {
    if (!s_ace_af) {
        fprintf(stderr, "gpu ace selftest: SKIP (this device has no anisotropic filtering)\n");
        return 77;
    }
    if (s_tex_pool_sets != 2u * MAX_TEX) {
        fprintf(stderr, "gpu ace selftest: FAIL texture pool has %u sets, expected %u\n",
                s_tex_pool_sets, 2u * MAX_TEX);
        return 0;
    }
    if (!coherence_run_psp_sample_points()) return 0;
    if (s_cnt_af_samplers != 0u) {
        fprintf(stderr, "gpu ace selftest: FAIL through-mode draws made %llu anisotropic samplers\n",
                (unsigned long long)s_cnt_af_samplers);
        return 0;
    }
    /* The list left the 32x32 texture bound. Draw it transformed, then nearest, then through. */
    GeState *g = s_ge;
    const uint32_t through = 1u << 23;
    g->vtype &= ~through;
    g->tex_filter = 0x101u;                                  /* linear min and mag */
    VkDescriptorSet aniso = tex_get();
    VkDescriptorSet again = tex_get();
    int ok = s_cnt_af_samplers == 1u && aniso == again && aniso != s_white_set;
    if (!ok) fprintf(stderr, "gpu ace selftest: FAIL transformed linear draw: samplers=%llu same=%d\n",
                     (unsigned long long)s_cnt_af_samplers, aniso == again);
    g->tex_filter = 0x100u;                                  /* nearest min, linear mag */
    VkDescriptorSet nearest = tex_get();
    if (ok && (s_cnt_af_samplers != 1u || nearest == aniso)) {
        fprintf(stderr, "gpu ace selftest: FAIL nearest minification got anisotropy\n");
        ok = 0;
    }
    g->tex_filter = 0x101u;
    g->vtype |= through;
    VkDescriptorSet plain = tex_get();
    if (ok && (s_cnt_af_samplers != 1u || plain == aniso)) {
        fprintf(stderr, "gpu ace selftest: FAIL a through-mode draw bound the anisotropic set\n");
        ok = 0;
    }
    if (ok) printf("gpu ace selftest: PASS af=%d through-mode exact, transformed linear filtered, "
                   "nearest kept\n", s_ace_af);
    return ok;
}

int gegpu_snapshot_sync_selftest(void) {
    const uint32_t fba = 0x00140000u;
    const uint32_t base = 0x04000000u | fba;
    const uint32_t stride = 512u, fmt = 3u;
    const uint32_t bytes = stride * FB_H * 4u;
    coherence_reset_targets();
    uint8_t *guest = (uint8_t *)SR_HOST(base);
    coherence_fill_raw(guest, stride * FB_H, 4u, coherence_g1(fmt));
    Target *t = target_color_acquire(fba, stride, fmt, 1);
    if (!t || !t->gpu_valid) {
        fprintf(stderr, "gpu snapshot sync selftest: could not establish GPU target\n");
        coherence_reset_targets();
        return 0;
    }

    /* The image remains GPU G1, but guest VRAM is deliberately restored to stale G0. */
    coherence_fill_raw(guest, stride * FB_H, 4u, coherence_g0(fmt));
    t->clean_gen = 0;
    if (!target_prepare_present(t)) {
        fprintf(stderr, "gpu snapshot sync selftest: present readback setup failed\n");
        coherence_reset_targets();
        return 0;
    }

    ReadbackSlot *pending = NULL;
    for (int i = 0; i < READBACK_FRAMES; i++) {
        if (s_readback[i].pending && s_readback[i].image == t->img) {
            pending = &s_readback[i];
            break;
        }
    }
    int stale = pending && pending->commit && t->clean_gen != t->render_gen &&
                memcmp(guest, &(uint32_t){ coherence_g0(fmt) }, sizeof(uint32_t)) == 0;
    if (!stale) {
        fprintf(stderr, "gpu snapshot sync selftest: no stale pre-commit snapshot observed "
                        "(render_gen=%llu clean_gen=%llu pending=%d)\n",
                (unsigned long long)t->render_gen, (unsigned long long)t->clean_gen,
                pending != NULL);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: ordinary_present_lag render_gen=%llu "
           "clean_gen=%llu pending=1 bytes=%u\n",
           (unsigned long long)t->render_gen, (unsigned long long)t->clean_gen, bytes);

    GeGpuFbDescriptor d = { base, fmt, stride, 480u, FB_H };
    int rc = gegpu_sync_guest_fb(&d);
    if (rc != GEGPU_SYNC_OK || t->clean_gen != t->render_gen ||
        memcmp(guest, &(uint32_t){ coherence_g1(fmt) }, sizeof(uint32_t)) != 0) {
        fprintf(stderr, "gpu snapshot sync selftest: explicit sync failed rc=%d "
                        "clean_gen=%llu render_gen=%llu\n", rc,
                (unsigned long long)t->clean_gen, (unsigned long long)t->render_gen);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: explicit_sync_ok render_gen=%llu\n",
           (unsigned long long)t->render_gen);

    GeGpuFbDescriptor bad = d;
    bad.width = 0;
    if (gegpu_sync_guest_fb(&bad) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: invalid descriptor was accepted\n");
        coherence_reset_targets();
        return 0;
    }
    GeGpuFbDescriptor cross = d;
    cross.addr = 0x041f0000u;
    if (gegpu_sync_guest_fb(&cross) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: cross-aperture descriptor was accepted\n");
        coherence_reset_targets();
        return 0;
    }

    /* A caller that agrees on the address but disagrees on how the bytes are laid
     * out would misread the buffer it just synchronised, so the boundary refuses.
     * These two controls keep that refusal honest.  A real observer defect once
     * produced exactly this shape -- live target 5551, caller describing 8888 --
     * and the refusal is what surfaced it.  Any change that makes an observer
     * quieter must leave these failing closed. */
    GeGpuFbDescriptor wrong_fmt = d;
    wrong_fmt.format = fmt == 3u ? 1u : 3u;
    if (gegpu_sync_guest_fb(&wrong_fmt) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: pixel-format mismatch against a live "
                        "target was accepted (target fmt=%u, caller fmt=%u)\n",
                fmt, wrong_fmt.format);
        coherence_reset_targets();
        return 0;
    }
    GeGpuFbDescriptor wrong_stride = d;
    wrong_stride.stride = stride + 64u;
    if (gegpu_sync_guest_fb(&wrong_stride) != GEGPU_SYNC_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: stride mismatch against a live "
                        "target was accepted (target stride=%u, caller stride=%u)\n",
                stride, wrong_stride.stride);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: live_target_mismatch_refused fmt and stride\n");
    GeGpuFbDescriptor no_target = d;
    no_target.addr = 0x04010000u;
    if (gegpu_sync_guest_fb(&no_target) != GEGPU_SYNC_NO_TARGET) {
        fprintf(stderr, "gpu snapshot sync selftest: unmapped VRAM was not NO_TARGET\n");
        coherence_reset_targets();
        return 0;
    }
    GeGpuFbDescriptor ram = d;
    ram.addr = 0x08010000u;
    if (gegpu_sync_guest_fb(&ram) != GEGPU_SYNC_NO_TARGET) {
        fprintf(stderr, "gpu snapshot sync selftest: main RAM was not NO_TARGET\n");
        coherence_reset_targets();
        return 0;
    }
    /* Declines that involve no GPU-valid target keep -1, so gui_present does not sync guest
     * VRAM for them. Only a GPU-valid target that fails to prepare or present reports
     * GEGPU_PRESENT_TARGET_FAILED (issue #827). */
    if (gegpu_present(0x08010000u, (int)fmt, stride) != GEGPU_PRESENT_NOT_RESIDENT ||
        gegpu_present(base, (int)fmt, FB_W + 64u) != GEGPU_PRESENT_NOT_RESIDENT) {
        fprintf(stderr, "gpu snapshot sync selftest: a present decline without a GPU target was not -1\n");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_declines_without_target_keep_code\n");

    if (!coherence_queue_g1_batch(base, stride, fmt) || !submit_pending() ||
        !target_prepare_present(t) || t->clean_gen == t->render_gen) {
        fprintf(stderr, "gpu snapshot sync selftest: ordinary second present lost async lag\n");
        coherence_reset_targets();
        return 0;
    }
    if (gegpu_sync_guest_fb(&d) != GEGPU_SYNC_OK || t->clean_gen != t->render_gen) {
        fprintf(stderr, "gpu snapshot sync selftest: second explicit sync failed\n");
        coherence_reset_targets();
        return 0;
    }

    /* Present-side failure (issue #827): the target is GPU-valid and newer than guest VRAM.
     * The sdl3vk fault seam (selftest only) makes vkQueuePresentKHR report OUT_OF_DATE, which
     * present_common resolves as -1 after the target was prepared. gegpu_present() must report
     * GEGPU_PRESENT_TARGET_FAILED, and the sync gui_present runs before its fall-through must
     * make guest VRAM current with the newest generation. */
    coherence_fill_raw(guest, stride * FB_H, 4u, coherence_g0(fmt));
    if (!coherence_queue_g1_batch(base, stride, fmt) || !submit_pending() ||
        t->clean_gen == t->render_gen) {
        fprintf(stderr, "gpu snapshot sync selftest: could not stage a newer GPU target\n");
        coherence_reset_targets();
        return 0;
    }
    sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_DATE_KHR);
    int failed_present = gegpu_present(base, (int)fmt, stride);
    sdl3vk_present_fault_clear();
    if (failed_present != GEGPU_PRESENT_TARGET_FAILED) {
        fprintf(stderr, "gpu snapshot sync selftest: failed present of a prepared target returned %d, "
                        "not GEGPU_PRESENT_TARGET_FAILED\n", failed_present);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_target_failure_reports_target_failed\n");
    if (gegpu_sync_guest_fb(&d) != GEGPU_SYNC_OK || t->clean_gen != t->render_gen ||
        memcmp(guest, &(uint32_t){ coherence_g1(fmt) }, sizeof(uint32_t)) != 0) {
        fprintf(stderr, "gpu snapshot sync selftest: guest VRAM was not current after a target failure\n");
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_target_failure_synced_guest_vram\n");

    /* Controls: a present that is shown, and one that is shown with an enqueued swapchain
     * rebuild (VK_SUBOPTIMAL_KHR), both still report 1. Only -1 is remapped. */
    sdl3vk_present_fault_inject(VK_SUBOPTIMAL_KHR);
    int rebuilt_present = gegpu_present(base, (int)fmt, stride);
    sdl3vk_present_fault_clear();
    if (rebuilt_present != GEGPU_PRESENT_SHOWN ||
        gegpu_present(base, (int)fmt, stride) != GEGPU_PRESENT_SHOWN) {
        fprintf(stderr, "gpu snapshot sync selftest: a shown present did not stay 1 (rebuild=%d)\n",
                rebuilt_present);
        coherence_reset_targets();
        return 0;
    }
    printf("gpu snapshot sync selftest: present_shown_stays_1\n");
    coherence_reset_targets();
    return 1;
}
#endif

void gegpu_ace_stats(GeGpuAceStats *out) {
    if (!out) return;
    out->af = s_ace_af;
    out->af_samplers = (unsigned long long)s_cnt_af_samplers;
    out->tex_pool_sets = s_tex_pool_sets;
}

int gegpu_init(void) {
    Sdl3VkInfo vi;
    sr_perf_set_gpu_replay_source(gpu_replay_snapshot);
    if (!sdl3vk_get_vk(&vi)) { fprintf(stderr, "gegpu: sdl3vk not initialized\n"); return 0; }
    s_pdev  = (VkPhysicalDevice)vi.physical;
    s_dev   = (VkDevice)vi.device;
    s_queue = (VkQueue)vi.queue;
    s_ge    = ge_state_ptr();
    s_zbuf  = ge_zbuf_ptr();
    {
        const char *lg = getenv("SR_GPU_LOG");
        s_log = (lg && lg[0] && strcmp(lg, "0") != 0) ? 1 : 0;
        const char *st = getenv("SR_GPU_STATS");
        s_stats = s_log || (st && st[0] && strcmp(st, "0") != 0);
        const char *cpu_profile = getenv("SR_GPU_CPU_PROFILE");
        s_cpu_profile = cpu_profile && cpu_profile[0] && strcmp(cpu_profile, "0") != 0;
        const char *shadow_disable = getenv("SR_GPU_TEX_SHADOW_DISABLE");
        s_tex_shadow_enabled = !(shadow_disable && shadow_disable[0] &&
                                 strcmp(shadow_disable, "0") != 0);
        const char *sync = getenv("SR_GPU_SYNC_SUBMIT");
        s_async_submit = !(sync && sync[0] && strcmp(sync, "0") != 0);
        s_submit_batch_ops = SUBMIT_BATCH_MAX;
        const char *batch = getenv("SR_GPU_SUBMIT_BATCH");
        if (batch && batch[0]) {
            long value = strtol(batch, NULL, 10);
            if (value >= 1 && value <= SUBMIT_BATCH_MAX) s_submit_batch_ops = (uint32_t)value;
        }
        if (!s_async_submit) s_submit_batch_ops = 1;
        s_xfer_ring_bytes = XFER_RING_DEFAULT_BYTES;
        const char *ring_kb = getenv("SR_GPU_XFER_RING_KB");
        if (ring_kb && ring_kb[0]) {
            unsigned long long value = strtoull(ring_kb, NULL, 10);
            if (value == 0) s_xfer_ring_bytes = 0;
            else if (value <= 65536) s_xfer_ring_bytes = (VkDeviceSize)value << 10;
        }
        s_scale = sr_parse_render_scale(getenv("SR_GPU_SCALE"),
                                        getenv("SR_RESOLUTION_SCALE"), MAX_SCALE);
        if (s_scale > 1)
            fprintf(stderr, "gegpu: render scale %dx (%ux%u internal)\n", s_scale, SCL_W, SCL_H);
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s_pdev, &props);
    s_xfer_align = props.limits.optimalBufferCopyOffsetAlignment;
    if (s_xfer_align < 4) s_xfer_align = 4;

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vi.queue_family;
    VKC(vkCreateCommandPool(s_dev, &cpi, NULL, &s_pool));
    VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = s_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (int i = 0; i < SUBMIT_FRAMES; i++) {
        SubmitSlot *slot = &s_submit[i];
        VKC(vkAllocateCommandBuffers(s_dev, &cbi, &slot->cmd));
        VKC(vkCreateFence(s_dev, &fci, NULL, &slot->fence));
        if (!make_buffer((VkDeviceSize)VERT_ARENA_VERTS * sizeof(GpuVert), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                         &slot->vbuf, &slot->vbuf_mem, (void **)&slot->vmap)) return 0;
        if (s_xfer_ring_bytes &&
            !make_buffer(s_xfer_ring_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         &slot->xfer, &slot->xfer_mem, (void **)&slot->xfer_map)) return 0;
    }

    /* render pass: LOAD/STORE both attachments; color ends SHADER_READ_ONLY so finished
     * targets are always samplable/presentable, depth stays an attachment */
    VkAttachmentDescription at[2] = {0};
    at[0].format = VK_FORMAT_R8G8B8A8_UNORM;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD; at[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    at[1] = at[0];
    at[1].format = VK_FORMAT_D16_UNORM;
    at[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference zref = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {0};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1; sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &zref;
    VkRenderPassCreateInfo rpc = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    rpc.attachmentCount = 2; rpc.pAttachments = at;
    rpc.subpassCount = 1; rpc.pSubpasses = &sub;
    VKC(vkCreateRenderPass(s_dev, &rpc, NULL, &s_rp));

    /* the transfer staging buffer must hold one full SCALED color image */
    VkDeviceSize xfer_bytes = (VkDeviceSize)SCL_W * SCL_H * 4u;
    if (xfer_bytes < (1u << 20)) xfer_bytes = 1u << 20;
    if (!make_buffer(xfer_bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     &s_xfer, &s_xfer_m, &s_xfer_map)) return 0;
    for (int i = 0; i < READBACK_FRAMES; i++) {
        ReadbackSlot *r = &s_readback[i];
        cbi.commandBufferCount = 1;
        VKC(vkAllocateCommandBuffers(s_dev, &cbi, &r->cmd));
        VKC(vkCreateFence(s_dev, &fci, NULL, &r->fence));
        if (!make_buffer((VkDeviceSize)SCL_W * SCL_H * 4u, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                         &r->buf, &r->mem, (void **)&r->map)) return 0;
    }

    /* binding 0 = the set's own texture, binding 1 = the shared destination snapshot
     * (shader blending); every set carries both so any set works with any pipeline */
    VkDescriptorSetLayoutBinding db[2] = {{0}, {0}};
    db[0].binding = 0; db[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    db[0].descriptorCount = 1; db[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    db[1] = db[0]; db[1].binding = 1;
    VkDescriptorSetLayoutCreateInfo dlc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dlc.bindingCount = 2; dlc.pBindings = db;
    VKC(vkCreateDescriptorSetLayout(s_dev, &dlc, NULL, &s_dlayout));

    /* Ace Mode's texture filtering (ace.h) can give every entry a second, anisotropic set; the
     * pool doubles for it then, and only then. */
    s_ace_af = g_ace_on ? ace_effective()->af : 0;
    const uint32_t tex_sets = s_ace_af ? 2u * MAX_TEX : MAX_TEX;
    s_tex_pool_sets = tex_sets;
    if (s_ace_af) ace_confirm(ACE_FEATURE_AF);   /* the renderer that filters is up */
    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2u * tex_sets };
    VkDescriptorPoolCreateInfo dpc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpc.maxSets = tex_sets; dpc.poolSizeCount = 1; dpc.pPoolSizes = &dps;
    dpc.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;   /* LRU eviction */
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_dpool_tex));
    VkDescriptorPoolSize dpsf = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  2u * FIXED_FILTER_DESCRIPTOR_SETS };
    dpc.maxSets = FIXED_FILTER_DESCRIPTOR_SETS; dpc.pPoolSizes = &dpsf;
    dpc.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_dpool_fix));

    /* Set 1: the GPU T&L parameter arena (#318). Every pipeline shares the one layout; only
     * psp_tnl.vert reads set 1, and only its draws bind it. */
    VkDescriptorSetLayoutBinding tdb = {0};
    tdb.binding = 0; tdb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    tdb.descriptorCount = 1; tdb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo tdlc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    tdlc.bindingCount = 1; tdlc.pBindings = &tdb;
    VKC(vkCreateDescriptorSetLayout(s_dev, &tdlc, NULL, &s_tnl_dlayout));
    VkDescriptorPoolSize tdps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SUBMIT_FRAMES };
    VkDescriptorPoolCreateInfo tdpc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    tdpc.maxSets = SUBMIT_FRAMES; tdpc.poolSizeCount = 1; tdpc.pPoolSizes = &tdps;
    VKC(vkCreateDescriptorPool(s_dev, &tdpc, NULL, &s_tnl_dpool));

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushPC) };
    VkDescriptorSetLayout set_layouts[2] = { s_dlayout, s_tnl_dlayout };
    VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plc.setLayoutCount = 2; plc.pSetLayouts = set_layouts;
    plc.pushConstantRangeCount = 1; plc.pPushConstantRanges = &pcr;
    VKC(vkCreatePipelineLayout(s_dev, &plc, NULL, &s_playout));

    VkShaderModuleCreateInfo smc = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smc.codeSize = sizeof(k_vert_spv); smc.pCode = k_vert_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_vs));
    smc.codeSize = sizeof(k_frag_spv); smc.pCode = k_frag_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_fs));
    /* psp_tnl.vert declares DenormPreserve (it must keep fp32 subnormals to match ge.c bit for
     * bit), which a device may run only with VK_KHR_shader_float_controls and
     * shaderDenormPreserveFloat32. Without them there is no T&L stage: hook_tnl_prim declines
     * and ge.c keeps T&L. */
    s_vs_tnl = VK_NULL_HANDLE;
    if (vi.denorm_preserve_f32) {
        smc.codeSize = sizeof(k_tnl_vert_spv); smc.pCode = k_tnl_vert_spv;
        VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_vs_tnl));
    }

    /* Shared clamp samplers for render-target and snapshot sampling. The min and mag
     * fields stay independent so Vulkan selects the PSP TFLT field from the footprint. */
    {
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        /* Preserve minification selection without exposing any mip level beyond level 0. */
        sci.maxLod = 0.25f;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        for (unsigned i = 0; i < TEX_FILTER_VARIANTS; i++) {
            sci.magFilter = (i & 1u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            sci.minFilter = (i & 2u) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
            VKC(vkCreateSampler(s_dev, &sci, NULL, &s_smp[i]));
        }
    }

    /* snapshot image for feedback (self-sampling) draws and the shader-blend dst read.
     * Created BEFORE any descriptor set: make_descriptor writes it into binding 1 of
     * every set it allocates. */
    if (!make_image(SCL_W, SCL_H, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    &s_snapimg, &s_snapimg_mem)) return 0;
    if (!make_view(s_snapimg, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_view)) return 0;
    s_snap_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    /* the image sits in every set's binding 1 from the first draw on; move it out of
     * UNDEFINED now so early draws (before the first snapshot copy) are valid */
    if (!cmd_begin()) return 0;
    to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
              VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (!cmd_submit_wait(SR_PERF_GE_INIT)) return 0;
    for (unsigned i = 0; i < TEX_FILTER_VARIANTS; i++) {
        s_snap_set[i] = make_descriptor(s_snap_view, s_smp[i], s_dpool_fix);
        if (!s_snap_set[i]) return 0;
    }

    /* 1x1 white texture for untextured draws */
    {
        uint32_t white = 0xFFFFFFFFu;
        static const TexMipKey no_mip;
        VkImageLayout white_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!tex_make(1, 1, 1u, 0, 0, 1, 1, &no_mip,
                      &s_white, &s_white_mem, &s_white_view, &s_white_smp) ||
            !tex_upload_level(s_white, &white_layout, 0u, 1, &white, 1, 1))
            return 0;
        s_white_set = make_descriptor(s_white_view, s_white_smp, s_dpool_fix);
        if (!s_white_set) return 0;
    }

    s_readback_commit_failures = 0;
    s_ready = 1;
    ge_set_gpu_hooks(&k_hooks);
    fprintf(stderr, "gegpu: full GPU GE active (persistent targets, %s, no software fallback)\n",
            s_async_submit ? "8-slot async render/snapshot batching" : "exact synchronous submits");
    if (s_cpu_profile)
        fprintf(stderr, "gegpu: aggregate CPU profiling enabled\n");
    fprintf(stderr, "gegpu: exact texture byte-shadow reuse %s (bounded at %u MiB)\n",
            s_tex_shadow_enabled ? "enabled" : "disabled", TEX_SHADOW_MAX_BYTES >> 20);
    if (s_async_submit) {
        if (s_xfer_ring_bytes)
            fprintf(stderr, "gegpu: command-buffer batch limit=%u upload-ring=%llu KiB/slot alignment=%llu\n",
                    s_submit_batch_ops, (unsigned long long)(s_xfer_ring_bytes >> 10),
                    (unsigned long long)s_xfer_align);
        else
            fprintf(stderr, "gegpu: command-buffer batch limit=%u upload-ring=disabled (exact shared-xfer boundaries)\n",
                    s_submit_batch_ops);
    }
    return 1;
}

void gegpu_shutdown(void) {
    if (!s_ready) return;
    submit_pending();
    (void)cmd_drain(GEGPU_BOUNDARY_LIFETIME);
    for (int i = 0; i < READBACK_FRAMES; i++)
        (void)readback_finish(&s_readback[i], 1, 0);
    /* Presentation uses the same device but owns separate fences/semaphores. Shutdown is
     * a real resource-lifetime boundary, so retire all device work before destroying any
     * object that a recorded command or descriptor can reference. */
    (void)vkDeviceWaitIdle(s_dev);
    stats_emit(1);
    ge_set_gpu_hooks(NULL);
    s_ready = 0;

    for (int i = 0; i < MAX_TGT; i++) target_destroy(&s_tgts[i]);
    for (int i = 0; i < MAX_DEP; i++) {
        DepthEnt *d = &s_deps[i];
        if (!d->used) continue;
        if (d->view) vkDestroyImageView(s_dev, d->view, NULL);
        if (d->img) vkDestroyImage(s_dev, d->img, NULL);
        if (d->mem) vkFreeMemory(s_dev, d->mem, NULL);
        memset(d, 0, sizeof(*d));
    }
    for (int i = 0; i < s_tex_n; i++) {
        TexEnt *e = &s_tex[i];
        tex_shadow_release(e);
        if (e->set) vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set);
        if (e->smp) vkDestroySampler(s_dev, e->smp, NULL);
        if (e->set_af) vkFreeDescriptorSets(s_dev, s_dpool_tex, 1, &e->set_af);
        if (e->smp_af) vkDestroySampler(s_dev, e->smp_af, NULL);
        if (e->view) vkDestroyImageView(s_dev, e->view, NULL);
        if (e->img) vkDestroyImage(s_dev, e->img, NULL);
        if (e->mem) vkFreeMemory(s_dev, e->mem, NULL);
    }
    s_tex_n = 0;
    for (int i = 0; i < s_pipe_n; i++)
        if (s_pipes[i].pipe) vkDestroyPipeline(s_dev, s_pipes[i].pipe, NULL);
    s_pipe_n = 0;
    free(s_pipes);
    s_pipes = NULL;
    s_pipe_cap = 0;

    if (s_white_set) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &s_white_set);
    for (unsigned i = 0; i < TEX_FILTER_VARIANTS; i++)
        if (s_snap_set[i]) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &s_snap_set[i]);
    if (s_dpool_tex) vkDestroyDescriptorPool(s_dev, s_dpool_tex, NULL);
    if (s_dpool_fix) vkDestroyDescriptorPool(s_dev, s_dpool_fix, NULL);

    if (s_white_smp) vkDestroySampler(s_dev, s_white_smp, NULL);
    if (s_white_view) vkDestroyImageView(s_dev, s_white_view, NULL);
    if (s_white) vkDestroyImage(s_dev, s_white, NULL);
    if (s_white_mem) vkFreeMemory(s_dev, s_white_mem, NULL);
    if (s_snap_view) vkDestroyImageView(s_dev, s_snap_view, NULL);
    if (s_snapimg) vkDestroyImage(s_dev, s_snapimg, NULL);
    if (s_snapimg_mem) vkFreeMemory(s_dev, s_snapimg_mem, NULL);
    for (unsigned i = 0; i < TEX_FILTER_VARIANTS; i++)
        if (s_smp[i]) vkDestroySampler(s_dev, s_smp[i], NULL);

    if (s_xfer_map) vkUnmapMemory(s_dev, s_xfer_m);
    if (s_xfer) vkDestroyBuffer(s_dev, s_xfer, NULL);
    if (s_xfer_m) vkFreeMemory(s_dev, s_xfer_m, NULL);
    for (int i = 0; i < SUBMIT_FRAMES; i++) {
        SubmitSlot *slot = &s_submit[i];
        if (slot->vmap) vkUnmapMemory(s_dev, slot->vbuf_mem);
        if (slot->vbuf) vkDestroyBuffer(s_dev, slot->vbuf, NULL);
        if (slot->vbuf_mem) vkFreeMemory(s_dev, slot->vbuf_mem, NULL);
        if (slot->xfer_map) vkUnmapMemory(s_dev, slot->xfer_mem);
        if (slot->xfer) vkDestroyBuffer(s_dev, slot->xfer, NULL);
        if (slot->xfer_mem) vkFreeMemory(s_dev, slot->xfer_mem, NULL);
        if (slot->tmap) vkUnmapMemory(s_dev, slot->tbuf_mem);
        if (slot->tbuf) vkDestroyBuffer(s_dev, slot->tbuf, NULL);
        if (slot->tbuf_mem) vkFreeMemory(s_dev, slot->tbuf_mem, NULL);
        if (slot->pmap) vkUnmapMemory(s_dev, slot->pbuf_mem);
        if (slot->pbuf) vkDestroyBuffer(s_dev, slot->pbuf, NULL);
        if (slot->pbuf_mem) vkFreeMemory(s_dev, slot->pbuf_mem, NULL);
        if (slot->fence) vkDestroyFence(s_dev, slot->fence, NULL);
        memset(slot, 0, sizeof(*slot));
    }
    for (int i = 0; i < READBACK_FRAMES; i++) {
        ReadbackSlot *r = &s_readback[i];
        if (r->map) vkUnmapMemory(s_dev, r->mem);
        if (r->buf) vkDestroyBuffer(s_dev, r->buf, NULL);
        if (r->mem) vkFreeMemory(s_dev, r->mem, NULL);
        if (r->fence) vkDestroyFence(s_dev, r->fence, NULL);
        memset(r, 0, sizeof(*r));
    }

    if (s_vs) vkDestroyShaderModule(s_dev, s_vs, NULL);
    if (s_fs) vkDestroyShaderModule(s_dev, s_fs, NULL);
    if (s_vs_tnl) vkDestroyShaderModule(s_dev, s_vs_tnl, NULL);
    if (s_tnl_dpool) vkDestroyDescriptorPool(s_dev, s_tnl_dpool, NULL);
    if (s_tnl_dlayout) vkDestroyDescriptorSetLayout(s_dev, s_tnl_dlayout, NULL);
    s_vs_tnl = VK_NULL_HANDLE; s_tnl_dpool = VK_NULL_HANDLE; s_tnl_dlayout = VK_NULL_HANDLE;
    free(s_tnl_vcpu); free(s_tnl_pcpu); free(s_tnl_box);
    s_tnl_vcpu = NULL; s_tnl_pcpu = NULL; s_tnl_box = NULL;
    s_tnl_ready = 0; s_ntnl = s_ntnlp = 0;
    if (s_playout) vkDestroyPipelineLayout(s_dev, s_playout, NULL);
    if (s_rp) vkDestroyRenderPass(s_dev, s_rp, NULL);
    if (s_dlayout) vkDestroyDescriptorSetLayout(s_dev, s_dlayout, NULL);
    if (s_pool) vkDestroyCommandPool(s_dev, s_pool, NULL);

    s_cmd = VK_NULL_HANDLE;
    s_cmd_slot = NULL;
    s_snap_src = NULL;
    s_snap_srcgen = 0;
    s_cur = NULL;
}
