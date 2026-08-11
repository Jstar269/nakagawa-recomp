// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
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
 *  - 16-bit precision (5650/5551/4444 framebuffers): the fragment shader snaps its
 *    output to the native lattice (pack-by-truncation + expand-by-bit-replication,
 *    identical to ge.c pack_fb/unpack_color) and applies the GE's 4x4 ordered dither,
 *    so the RGBA8 target image round-trips guest VRAM bit-exactly — no color or
 *    depth drift across consecutive frames.
 *  - Blending: simple states map to Vulkan fixed function (doubled SRC-alpha is exact
 *    via shader premultiply; min/max are exact because both the PSP and Vulkan ignore
 *    the factors). Everything fixed function cannot express — doubled DST-alpha
 *    factors, absdiff, two distinct FIX constants, and any blend onto a 16-bit target
 *    (the hardware quantizes/dithers the blend RESULT on store) — runs as SHADER
 *    blending: the fragment shader reads a snapshot of the destination and evaluates
 *    ge.c's integer blend_chan()/factor_component() formulas verbatim. The snapshot is
 *    refreshed per state build (pending batches submit first), so the one remaining
 *    approximation is overlapping primitives INSIDE a single batch reading the
 *    pre-batch destination — the standard shader-blend hazard, same as PPSSPP.
 *  - Approximated (no software fallback): partial-byte write masks (>= 0x80 disables
 *    the channel), lines/points (1px quads, not DDA).
 *  - Render scale (SR_GPU_SCALE=1..4, default 1): targets/depth/snapshot images are
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
#define MAX_PIPES  192
#define MAX_TEX    1024
#define MAX_TGT    8
#define MAX_DEP    4
#define READBACK_FRAMES 3
#define SUBMIT_FRAMES 8
#define SUBMIT_BATCH_MAX 8
#define XFER_RING_DEFAULT_BYTES (8u << 20)
#define TEX_SHADOW_MAX_BYTES (64u << 20)
/* A slot can retain more than one recorded render before submission. Each render gets
 * a disjoint range; exhaustion submits early rather than overwriting vertices that an
 * earlier vkCmdDraw in the same command buffer still references. */
#define VERT_ARENA_VERTS (MAX_VERTS * 2u)

static const uint32_t k_vert_spv[] =
#include "psp_vert.inc"
;
static const uint32_t k_frag_spv[] =
#include "psp_frag.inc"
;

/* ---- captured vertex (must match psp.vert input layout) ------------------------------ */
typedef struct { float x, y, z, rw, u, v, fog; uint32_t rgba; } GpuVert;

/* fragment push constants (must match psp.frag PC block; 128 bytes = the guaranteed
 * VK minimum maxPushConstantsSize) */
typedef struct { int32_t cfg[4]; float texenv[4]; float fogcol[4]; float texsize[4];
                 int32_t dith[4]; int32_t bl[4]; float fixa[4]; float fixb[4]; } PushPC;
enum { F_TEX = 1, F_RGBA = 2, F_DBL = 4, F_FOG = 8, F_PERSP = 16, F_CLEAR = 32,
       F_NEAREST = 64, F_SA2X = 128, F_SA2XI = 256, F_DITHER = 512, F_SHBLEND = 4096 };
#define F_FMT_SHIFT 10                     /* bits 10-11: framebuffer psm for quantization */

typedef struct {
    uint8_t blend_on, srcf, dstf, eq;      /* VkBlendFactor / VkBlendOp values */
    uint8_t cull;                          /* VkCullModeFlagBits */
    uint8_t ztest, zwrite, zfunc;          /* VkCompareOp */
    uint8_t cmask;                         /* VkColorComponentFlags */
} PipeKey;

typedef struct {
    PipeKey key;
    int32_t sx, sy, sw, sh;                /* scissor rect */
    float   bconst[4];                     /* blend constants (FIX factors) */
    PushPC  pc;
    VkDescriptorSet dset;
    uint32_t first, count;
} Batch;

typedef struct {
    uint64_t key, hash;                    /* state key + content hash; 0,0 = empty */
    VkImage img; VkDeviceMemory mem; VkImageView view; VkSampler smp;
    VkDescriptorSet set;
    int w, h;
    uint32_t addr, bytes;                  /* guest source range for precise CPU-write invalidation */
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

/* shared depth buffer, keyed by guest zbuf placement */
typedef struct {
    int used;
    uint32_t zba, zstride;                 /* key: zbp & 0x1FFFFF, stride */
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
    int cpu_dirty;                         /* software fallback changed s_zbuf */
} DepthEnt;

/* persistent GE render target */
typedef struct {
    int used;
    uint32_t fba, stride, fmt;             /* key: framebuffer addr (masked), stride px, psm */
    VkImage img; VkDeviceMemory mem; VkImageView view;
    VkImageLayout layout;
    VkDescriptorSet set_n, set_l;          /* sampled (nearest / linear) for RTT + blit src */
    DepthEnt *dep;
    VkFramebuffer fb;                      /* for (this color, this->dep) */
    int gpu_valid;                         /* GPU contents are the truth (newer than VRAM) */
    uint64_t lru;
    uint64_t render_gen;                   /* bumped per submit that rendered into this */
    uint64_t clean_gen;                    /* render_gen when guest VRAM last matched */
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
    int in_flight;
} SubmitSlot;

/* ---- backend state -------------------------------------------------------------------- */
static int s_ready = 0;
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

static TexEnt  s_tex[MAX_TEX];
static int     s_tex_n = 0;
static VkImage s_white; static VkDeviceMemory s_white_mem;
static VkImageView s_white_view; static VkSampler s_white_smp;
static VkDescriptorSet s_white_set;

/* snapshot copy of a target for self-sampling (feedback) draws */
static VkImage s_snapimg; static VkDeviceMemory s_snapimg_mem;
static VkImageView s_snap_view; static VkImageLayout s_snap_layout;
static VkSampler s_smp_n, s_smp_l;         /* shared nearest/linear samplers (clamp) */
static VkDescriptorSet s_snap_n, s_snap_l;
static Target *s_snap_src = NULL;          /* what the snapshot currently holds */
static uint64_t s_snap_srcgen = 0;

static PipeEnt s_pipes[MAX_PIPES];
static int s_pipe_n = 0;

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
static uint64_t s_cnt_batches = 0, s_cnt_verts = 0;
unsigned long s_cnt_present_gpu = 0;

/* Public read-only accessor: number of GE list submissions since process start.
 * Unused by the recomp/runtime; kept available for downstream tools/diffing. */
unsigned long sr_submit_count(void) { return s_cnt_submit; }
static unsigned long s_cnt_present_cpu = 0, s_cnt_rtt = 0;
static unsigned long s_cnt_readback = 0, s_cnt_upload = 0, s_cnt_dirty = 0;
static unsigned long s_cnt_snap = 0, s_cnt_texup = 0, s_cnt_xferblit = 0;
static uint64_t s_cnt_tex_invalidations = 0;
static uint64_t s_cnt_shadow_checks = 0, s_cnt_shadow_hits = 0, s_cnt_shadow_misses = 0;
static uint64_t s_cnt_shadow_bytes = 0, s_cnt_shadow_avoided = 0, s_cnt_shadow_required = 0;
static GeGpuReplayStats s_replay_stats;
static GeGpuCpuProfileStats s_cpu_profile_stats;
static int s_cpu_profile;
static int s_cpu_profile_hook_depth;

static uint32_t s_texscratch[512 * 512];
static uint32_t s_pxscratch[FB_W * FB_H];

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

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf,
                       VkDeviceMemory *mem, void **map) {
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = size; bci.usage = usage;
    VKC(vkCreateBuffer(s_dev, &bci, NULL, buf));
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(s_dev, *buf, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VKC(vkAllocateMemory(s_dev, &mai, NULL, mem));
    VKC(vkBindBufferMemory(s_dev, *buf, *mem, 0));
    if (map) VKC(vkMapMemory(s_dev, *mem, 0, VK_WHOLE_SIZE, 0, map));
    return 1;
}

static int make_image(uint32_t w, uint32_t h, VkFormat fmt, VkImageUsageFlags usage,
                      VkImage *img, VkDeviceMemory *mem) {
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    ici.imageType = VK_IMAGE_TYPE_2D; ici.format = fmt;
    ici.extent.width = w; ici.extent.height = h; ici.extent.depth = 1;
    ici.mipLevels = 1; ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT; ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = usage; ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKC(vkCreateImage(s_dev, &ici, NULL, img));
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(s_dev, *img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VKC(vkAllocateMemory(s_dev, &mai, NULL, mem));
    VKC(vkBindImageMemory(s_dev, *img, *mem, 0));
    return 1;
}

static int make_view(VkImage img, VkFormat fmt, VkImageAspectFlags aspect, VkImageView *view) {
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    vci.image = img; vci.viewType = VK_IMAGE_VIEW_TYPE_2D; vci.format = fmt;
    vci.subresourceRange.aspectMask = aspect;
    vci.subresourceRange.levelCount = 1; vci.subresourceRange.layerCount = 1;
    VKC(vkCreateImageView(s_dev, &vci, NULL, view));
    return 1;
}

/* Transition an image whose layout we track to `want` (full barrier; correctness over
 * fine-grained sync — these happen a handful of times per frame). */
static void to_layout(VkCommandBuffer cmd, VkImage img, VkImageAspectFlags aspect,
                      VkImageLayout *cur, VkImageLayout want) {
    if (*cur == want) return;
    VkImageMemoryBarrier mb = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    mb.oldLayout = *cur; mb.newLayout = want;
    mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange.aspectMask = aspect;
    mb.subresourceRange.levelCount = 1;
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
    sr_perf_ge_wait(perf_started, slot->reason);
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
    sr_perf_ge_wait(perf_started, wait_reason);
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
    slot->xfer_used = 0;
    s_cmd_slot = slot;
    s_cmd = slot->cmd;
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKC(vkBeginCommandBuffer(s_cmd, &bi));
    return 1;
}

static int cmd_finish_submit(SrPerfGeReason reason, int defer) {
    SubmitSlot *slot = s_cmd_slot;
    if (!slot) return 0;
    VKC(vkEndCommandBuffer(s_cmd));
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &s_cmd;
    uint64_t submit_started = SDL_GetTicksNS();
    VKC(vkQueueSubmit(s_queue, 1, &si, slot->fence));
    replay_note_submit(reason, SDL_GetTicksNS() - submit_started);
    sr_perf_ge_submit(reason);
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

static int cmd_batch_begin(uint32_t vertices) {
    if (vertices > VERT_ARENA_VERTS) return 0;
    if (s_cmd_slot && (s_cmd_slot->recorded_ops >= s_submit_batch_ops ||
                       s_cmd_slot->vused + vertices > VERT_ARENA_VERTS)) {
        if (!cmd_batch_flush()) return 0;
    }
    if (!s_cmd_slot && !cmd_begin_fresh()) return 0;
    return 1;
}

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
    uint32_t row_pitch, last_row, visible_row, total;
    if (!sr_size_mul_ok(desc->stride, bpp, &row_pitch)) {
        *why = "row pitch overflows"; return 0;
    }
    if (!sr_size_mul_ok(desc->height - 1u, row_pitch, &last_row)) {
        *why = "final row offset overflows"; return 0;
    }
    if (!sr_size_mul_ok(desc->width, bpp, &visible_row) ||
        !sr_size_add_ok(last_row, visible_row, &total) || total == 0u) {
        *why = "visible span overflows"; return 0;
    }

    int in_vram = is_vram(desc->addr);
    uint32_t voff = 0u, base = desc->addr;
    if (in_vram) {
        voff = vram_off(desc->addr);
        if (voff >= GEGPU_VRAM_BYTES || total > GEGPU_VRAM_BYTES - voff) {
            *why = "span crosses the end of the VRAM aperture"; return 0;
        }
        base = 0x04000000u | voff;
    }
    if (!sr_guest_span_readable(base, total)) {
        *why = "span leaves guest memory"; return 0;
    }
    if (out_span) {
        out_span->base = base;
        out_span->bytes_per_pixel = bpp;
        out_span->row_pitch = row_pitch;
        out_span->total_bytes = total;
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
    if (wait) sr_perf_ge_wait(wait_started, SR_PERF_GE_TARGET_READBACK_TRANSITION);
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
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO },
    };
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;   st[0].module = s_vs; st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT; st[1].module = s_fs; st[1].pName = "main";

    VkVertexInputBindingDescription bind = { 0, sizeof(GpuVert), VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription at[4] = {
        { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
        { 1, 0, VK_FORMAT_R32G32_SFLOAT,       16 },
        { 2, 0, VK_FORMAT_R32_SFLOAT,          24 },
        { 3, 0, VK_FORMAT_R8G8B8A8_UNORM,      28 },
    };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    vi.vertexBindingDescriptionCount = 1;   vi.pVertexBindingDescriptions = &bind;
    vi.vertexAttributeDescriptionCount = 4; vi.pVertexAttributeDescriptions = at;

    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    vp.viewportCount = 1; vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = k->cull;
    /* raster_tri keeps POSITIVE cross product in y-down screen coords; Vulkan's signed
     * area has the opposite sign convention, so those triangles are Vulkan-"clockwise". */
    rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    rs.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
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
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    cb.attachmentCount = 1; cb.pAttachments = &ba;

    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS };
    VkPipelineDynamicStateCreateInfo dn = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    dn.dynamicStateCount = 3; dn.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo pci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    pci.stageCount = 2; pci.pStages = st;
    pci.pVertexInputState = &vi; pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp; pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms; pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cb; pci.pDynamicState = &dn;
    pci.layout = s_playout; pci.renderPass = s_rp;

    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &pci, NULL, &p);
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
    if (s_pipe_n >= MAX_PIPES) return VK_NULL_HANDLE;
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
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
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
    VkDescriptorImageInfo dds = { s_smp_n, s_snap_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr[2] = {
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
        { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET },
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
    s_flushgen++;
    for (int i = 0; i < s_tex_n; i++) s_tex[i].pending = 0;
}

/* Render all pending batches into s_cur. Leaves the color image SHADER_READ_ONLY. */
static int submit_pending(void) {
    if (!s_nbatch || !s_cur) { if (s_nbatch == 0) batches_reset(); return 1; }
    Target *t = s_cur;
    if (!cmd_batch_begin(s_nverts)) { batches_reset(); return 0; }
    uint32_t vertex_base = s_cmd_slot->vused;
    uint64_t memcpy_started = cpu_profile_now();
    memcpy(s_cmd_slot->vmap + vertex_base, s_vcpu, (size_t)s_nverts * sizeof(GpuVert));
    if (s_cpu_profile) {
        cpu_profile_add(GEGPU_CPU_MEMCPY, memcpy_started);
        s_cpu_profile_stats.memcpy_bytes += (uint64_t)s_nverts * sizeof(GpuVert);
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

    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rbi.renderPass = s_rp; rbi.framebuffer = t->fb;
    rbi.renderArea.extent.width = SCL_W; rbi.renderArea.extent.height = SCL_H;
    vkCmdBeginRenderPass(s_cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    /* the vertex NDC transform is resolution-independent: scaling the viewport IS the
     * internal-resolution upscale */
    VkViewport vp = { 0, 0, (float)SCL_W, (float)SCL_H, 0.0f, 1.0f };
    vkCmdSetViewport(s_cmd, 0, 1, &vp);
    VkDeviceSize vertex_offset = (VkDeviceSize)vertex_base * sizeof(GpuVert);
    vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_cmd_slot->vbuf, &vertex_offset);

    VkPipeline cur = VK_NULL_HANDLE;
    VkDescriptorSet cur_set = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < s_nbatch; i++) {
        Batch *b = &s_batch[i];
        VkPipeline p = pipe_get(&b->key);
        if (!p) continue;
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
        vkCmdDraw(s_cmd, b->count, 1, b->first, 0);
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

    s_replay_stats.batches += s_nbatch;
    s_replay_stats.draws += s_nbatch;
    s_cmd_slot->vused += s_nverts;
    int ok = cmd_batch_record(SR_PERF_GE_RENDER_BATCH);
    t->gpu_valid = 1;
    t->render_gen++;
    s_cnt_submit++;
    if (s_stats) {
        s_cnt_batches += s_nbatch;
        s_cnt_verts += s_nverts;
    }
    for (uint32_t i = 0; i < s_nbatch; i++) {
        uint32_t flags = (uint32_t)s_batch[i].pc.cfg[0] >> 8;
        if (flags & F_SHBLEND) sr_perf_ge_event(SR_PERF_GE_SHBLEND_BATCH, 1);
    }
    if (s_log)
        fprintf(stderr, "GEGPU submit #%lu batches=%u verts=%u tgt=0x%08x/%u fmt=%u\n",
                s_cnt_submit, s_nbatch, s_nverts, t->fba, t->stride, t->fmt);
    batches_reset();
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

static void stats_emit(int final) {
    static unsigned long last_ms = 0;
    if (!s_stats) return;
    unsigned long now = (unsigned long)(clock() * 1000ull / CLOCKS_PER_SEC);
    if (!final && now - last_ms < 5000) return;
    last_ms = now;
    fprintf(stderr, "GEGPU stats: submits=%lu batches=%llu verts=%llu avg_batches=%.2f "
            "tris=%lu spr=%lu lines=%lu rtt=%lu snap=%lu "
            "present[gpu=%lu cpu=%lu] upload=%lu readback=%lu texup=%lu dirty=%lu xferblit=%lu pipes=%d texs=%d "
            "shadow[invalidations=%llu checks=%llu hits=%llu misses=%llu bytes=%llu avoided=%llu required=%llu] final=%d\n",
            s_cnt_submit, (unsigned long long)s_cnt_batches, (unsigned long long)s_cnt_verts,
            s_cnt_submit ? (double)s_cnt_batches / (double)s_cnt_submit : 0.0,
            s_cnt_tri, s_cnt_spr, s_cnt_line, s_cnt_rtt, s_cnt_snap,
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
static int target_readback(Target *t) {
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
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
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
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &r->cmd;
    uint64_t submit_started = SDL_GetTicksNS();
    if (vkQueueSubmit(s_queue, 1, &si, r->fence) != VK_SUCCESS) {
        t->layout = old_layout;
        return 0;
    }
    replay_note_submit(SR_PERF_GE_TARGET_READBACK_TRANSITION,
                       SDL_GetTicksNS() - submit_started);
    sr_perf_ge_submit(SR_PERF_GE_TARGET_READBACK_TRANSITION);
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
    /* CPU VRAM is authoritative on this path.  Never let an older asynchronous copy
     * overwrite it after the upload has begun. */
    readback_discard_target(t);
    VkDeviceSize upload_bytes = (VkDeviceSize)SCL_W * SCL_H * 4u;
    UploadReservation upload;
    if (!upload_reserve(upload_bytes, &upload)) return 0;
    uint32_t *row = s_pxscratch;               /* one guest row of decode scratch */
    uint32_t *dst = (uint32_t *)upload.map;
    uint32_t base = 0x04000000u | t->fba;
    uint32_t drow = SCL_W;
    int oob = !sr_inrange(base);
    if (oob) fprintf(stderr, "target_upload: fba 0x%08x out of range, zeroing\n", t->fba);
    for (uint32_t y = 0; y < FB_H; y++) {
        if (oob) {
            memset(row, 0, t->stride * 4);
        } else if (t->fmt == 3) {
            memcpy(row, (const uint32_t *)SR_HOST(base + y * t->stride * 4), t->stride * 4);
        } else {
            const uint16_t *src = (const uint16_t *)SR_HOST(base + y * t->stride * 2);
            for (uint32_t x = 0; x < t->stride; x++) row[x] = fb_unpack(src[x], t->fmt);
        }
        for (uint32_t x = t->stride; x < FB_W; x++) row[x] = 0;
        uint32_t *dl = dst + (uint32_t)(y * s_scale) * drow;
        if (s_scale == 1) {
            memcpy(dl, row, FB_W * 4);
        } else {
            for (uint32_t x = 0; x < FB_W; x++)
                for (int i = 0; i < s_scale; i++) dl[x * s_scale + i] = row[x];
            for (int j = 1; j < s_scale; j++) memcpy(dl + (uint32_t)j * drow, dl, drow * 4);
        }
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
    uint32_t base = 0x04000000u | t->fba;
    for (uint32_t y = 0; y < FB_H; y++) {
        if (!row_used[y]) continue;
        uint32_t x0 = row_x0[y], x1 = row_x1[y];
        uint32_t scaled_width = (x1 - x0) * (uint32_t)s_scale;
        uint32_t *dst = (uint32_t *)(upload.map + used);
        uint64_t row0 = target0 + (uint64_t)y * row_bytes;
        const uint8_t *guest_row = (const uint8_t *)SR_HOST(base + y * row_bytes);
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
            for (int sx = 0; sx < s_scale; sx++) dst[dx + (uint32_t)sx] = rgba;
        }
        for (int sy = 1; sy < s_scale; sy++)
            memcpy(dst + (uint32_t)sy * scaled_width, dst,
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

static void target_destroy(Target *t) {
    if (!t->used) return;
    (void)cmd_drain(GEGPU_BOUNDARY_LIFETIME);
    /* Retire only submissions that actually reference this image. */
    (void)readback_wait_target(t, 0);
    (void)sdl3vk_wait_image((void *)t->img);
    if (s_snap_src == t) s_snap_src = NULL;
    if (t->fb) vkDestroyFramebuffer(s_dev, t->fb, NULL);
    if (t->set_n) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &t->set_n);
    if (t->set_l) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &t->set_l);
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
        t->set_n = make_descriptor(t->view, s_smp_n, s_dpool_fix);
        t->set_l = make_descriptor(t->view, s_smp_l, s_dpool_fix);
        if (!t->set_n || !t->set_l) { target_destroy(t); return NULL; }
    }

    if (seed_from_vram && !t->gpu_valid) {
        if (!target_upload(t)) return NULL;
    }
    t->lru = s_lru++;
    return t;
}

static int depth_from_cpu(DepthEnt *d) {
    VkDeviceSize upload_bytes = (VkDeviceSize)SCL_W * SCL_H * 2u;
    UploadReservation upload;
    if (!upload_reserve(upload_bytes, &upload)) return 0;
    if (s_scale == 1) {
        memcpy(upload.map, s_zbuf, FB_W * FB_H * 2);
    } else {
        uint16_t *dst = (uint16_t *)upload.map;
        uint32_t drow = SCL_W;
        for (uint32_t y = 0; y < FB_H; y++) {
            const uint16_t *src = s_zbuf + y * FB_W;
            uint16_t *dl = dst + (uint32_t)(y * s_scale) * drow;
            for (uint32_t x = 0; x < FB_W; x++)
                for (int i = 0; i < s_scale; i++) dl[x * s_scale + i] = src[x];
            for (int j = 1; j < s_scale; j++) memcpy(dl + (uint32_t)j * drow, dl, drow * 2);
        }
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
    if (s_scale == 1) {
        memcpy(s_zbuf, s_xfer_map, FB_W * FB_H * 2);
    } else {
        const uint16_t *src = (const uint16_t *)s_xfer_map;
        for (uint32_t y = 0; y < FB_H; y++)
            for (uint32_t x = 0; x < FB_W; x++)
                s_zbuf[y * FB_W + x] = src[(uint32_t)(y * s_scale) * SCL_W + x * s_scale];
    }
    d->cpu_dirty = 1;
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
    if (zstride > FB_W) zstride = FB_W;

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
        VkFramebufferCreateInfo fbc = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
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

static uint32_t tex_source_size(void) {
    uint32_t bpp_num;
    switch (s_ge->tex_fmt) {
        case 3: case 7: bpp_num = 32; break;   /* 8888 / CLUT32 */
        case 4: case 8: bpp_num = 4;  break;   /* CLUT4 / DXT1 */
        case 5: case 9: case 10: bpp_num = 8; break; /* CLUT8 / DXT3 / DXT5 */
        default: bpp_num = 16; break;
    }
    uint32_t stride = s_ge->tex_bufw ? s_ge->tex_bufw : (uint32_t)s_ge->tex_w;
    uint32_t w = s_ge->tex_w > 0 ? (uint32_t)s_ge->tex_w : 1u;
    uint32_t h = s_ge->tex_h > 0 ? (uint32_t)s_ge->tex_h : 1u;
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
        bytes = ((uint64_t)stride * h * bpp_num) >> 3;
    }
    if (bytes > 4u << 20) bytes = 4u << 20;
    return (uint32_t)bytes;
}

static const uint8_t *tex_source_ptr(uint32_t addr, uint32_t bytes) {
    if (!bytes || !sr_inrange(addr)) return NULL;
    uint64_t last = (uint64_t)addr + bytes - 1u;
    if (last > UINT32_MAX || !sr_inrange((uint32_t)last)) return NULL;
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
    if (!s_tex_shadow_enabled || !e->bytes) return;
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

static uint64_t tex_hash(void) {
    uint64_t profile_started = cpu_profile_now();
    uint64_t bytes = tex_source_size();
    if (!sr_inrange(s_ge->tex_addr)) {
        cpu_profile_add(GEGPU_CPU_OBJECT_LOOKUP, profile_started);
        return 0;
    }
    const uint8_t *p = (const uint8_t *)SR_HOST(s_ge->tex_addr);
    uint64_t h = 1469598103934665603ull;
    h = fnv64(h, &bytes, sizeof(bytes));
    uint64_t step = bytes / 64; if (step < 16) step = 16;
    for (uint64_t off = 0; off + 16 <= bytes; off += step) h = fnv64(h, p + off, 16);
    if (bytes >= 16) h = fnv64(h, p + bytes - 16, 16);
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
    h = fnv64(h, s_ge->clutram, sizeof(s_ge->clutram));
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
        vkDestroyImageView(s_dev, e->view, NULL);
        vkDestroyImage(s_dev, e->img, NULL);
        vkFreeMemory(s_dev, e->mem, NULL);
        *e = s_tex[--s_tex_n];
    }
}

static int tex_upload(VkImage img, const uint32_t *px, int w, int h) {
    VkDeviceSize upload_bytes = (VkDeviceSize)(uint32_t)w * (uint32_t)h * 4u;
    UploadReservation upload;
    if (!upload_reserve(upload_bytes, &upload)) return 0;
    uint64_t memcpy_started = cpu_profile_now();
    memcpy(upload.map, px, (size_t)upload_bytes);
    if (s_cpu_profile) {
        cpu_profile_add(GEGPU_CPU_MEMCPY, memcpy_started);
        s_cpu_profile_stats.memcpy_bytes += (uint64_t)upload_bytes;
    }
    VkImageLayout lay = VK_IMAGE_LAYOUT_UNDEFINED;
    to_layout(s_cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, &lay, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = (uint32_t)w; bic.imageExtent.height = (uint32_t)h; bic.imageExtent.depth = 1;
    bic.bufferOffset = upload.offset;
    vkCmdCopyBufferToImage(s_cmd, upload.buffer, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    to_layout(s_cmd, img, VK_IMAGE_ASPECT_COLOR_BIT, &lay, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return upload_finish(&upload, SR_PERF_GE_TEXTURE_UPLOAD);
}

static int tex_make(const uint32_t *px, int w, int h, int linear, int clamp_u, int clamp_v,
                    VkImage *img, VkDeviceMemory *mem, VkImageView *view, VkSampler *smp) {
    if (!make_image((uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, img, mem))
        return 0;
    if (!tex_upload(*img, px, w, h)) return 0;
    if (!make_view(*img, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, view)) return 0;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.minFilter = sci.magFilter;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = clamp_u ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeV = clamp_v ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    VKC(vkCreateSampler(s_dev, &sci, NULL, smp));
    return 1;
}

static VkDescriptorSet tex_get(void) {
    int w = s_ge->tex_w, h = s_ge->tex_h;
    if (w <= 0 || h <= 0 || w > 512 || h > 512) return s_white_set;
    int linear  = (s_ge->tex_filter & 1) || ((s_ge->tex_filter >> 8) & 1);
    int clamp_u = s_ge->tex_wrap & 1, clamp_v = (s_ge->tex_wrap >> 8) & 1;
    uint64_t key = (uint64_t)s_ge->tex_addr | ((uint64_t)s_ge->tex_fmt << 32)
                 | ((uint64_t)(uint32_t)w << 36) | ((uint64_t)(uint32_t)h << 46)
                 | ((uint64_t)s_ge->tex_swizzle << 56) | ((uint64_t)(unsigned)linear << 57)
                 | ((uint64_t)(unsigned)clamp_u << 58) | ((uint64_t)(unsigned)clamp_v << 59);
    key ^= (uint64_t)s_ge->tex_bufw << 16;
    if (s_ge->tex_fmt >= 4 && s_ge->tex_fmt <= 7)
        key ^= clut_hash() | 1;        /* distinct entry per (texture, palette) pair */
    uint64_t hash = tex_hash();
    uint64_t lookup_started = cpu_profile_now();
    for (int i = 0; i < s_tex_n; i++) {
        TexEnt *e = &s_tex[i];
        if (e->key != key) continue;
        cpu_profile_add(GEGPU_CPU_OBJECT_LOOKUP, lookup_started);
        if (s_cpu_profile) s_cpu_profile_stats.texture_hits++;
        e->lru = s_texlru++;
        if (e->content_valid && e->hash == hash) { e->pending = 1; return e->set; }
        if (!e->content_valid && e->hash == hash && tex_shadow_matches(e)) {
            e->content_valid = 1;
            e->pending = 1;
            s_cnt_shadow_avoided++;
            return e->set;
        }
        /* same texture state, new contents: update in place (cache stays bounded) */
        if (e->pending) submit_pending();
        if (!e->content_valid) s_cnt_shadow_required++;
        uint64_t decode_started = cpu_profile_now();
        ge_decode_tex_rgba(s_texscratch);
        cpu_profile_add(GEGPU_CPU_TEXTURE_DECODE, decode_started);
        if (!tex_upload(e->img, s_texscratch, e->w, e->h)) return s_white_set;
        e->hash = hash;
        e->content_valid = 1;
        tex_shadow_store(e);
        e->pending = 1;
        s_cnt_texup++;
        return e->set;
    }

    cpu_profile_add(GEGPU_CPU_OBJECT_LOOKUP, lookup_started);
    if (s_cpu_profile) s_cpu_profile_stats.texture_misses++;

    if (s_tex_n >= MAX_TEX) {
        submit_pending();   /* batches reference sets about to be freed */
        tex_evict_lru();
    }
    uint64_t decode_started = cpu_profile_now();
    ge_decode_tex_rgba(s_texscratch);
    cpu_profile_add(GEGPU_CPU_TEXTURE_DECODE, decode_started);
    TexEnt *e = &s_tex[s_tex_n];
    memset(e, 0, sizeof(*e));
    if (!tex_make(s_texscratch, w, h, linear, clamp_u, clamp_v, &e->img, &e->mem, &e->view, &e->smp))
        return s_white_set;
    e->set = make_descriptor(e->view, e->smp, s_dpool_tex);
    if (!e->set) return s_white_set;
    e->key = key; e->hash = hash;
    e->w = w; e->h = h;
    e->addr = s_ge->tex_addr;
    e->bytes = tex_source_size();
    e->content_valid = 1;
    tex_shadow_store(e);
    e->pending = 1;
    e->lru = s_texlru++;
    s_cnt_texup++;
    s_tex_n++;
    return e->set;
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
    sr_perf_ge_event(SR_PERF_GE_SNAPSHOT_REQUEST, 1);
    if (s_nbatch) submit_pending();
    uint64_t decision_started = cpu_profile_now();
    if (s_snap_src == src && s_snap_srcgen == src->render_gen) {
        cpu_profile_add(GEGPU_CPU_SNAPSHOT_DECISION, decision_started);
        sr_perf_ge_event(SR_PERF_GE_SNAPSHOT_CACHE_HIT, 1);
        return 1;
    }
    cpu_profile_add(GEGPU_CPU_SNAPSHOT_DECISION, decision_started);
    if (cmd_batch_begin(0)) {
        /* Slot recycling/fence retirement happens inside cmd_batch_begin(). Start the
         * CPU-only region timer afterwards so GPU waits stay in boundary telemetry. */
        uint64_t region_started = cpu_profile_now();
        to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy ic = {0};
        ic.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ic.srcSubresource.layerCount = 1;
        ic.dstSubresource = ic.srcSubresource;
        ic.extent.width = SCL_W; ic.extent.height = SCL_H; ic.extent.depth = 1;
        vkCmdCopyImage(s_cmd, src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s_snapimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        to_layout(s_cmd, s_snapimg, VK_IMAGE_ASPECT_COLOR_BIT, &s_snap_layout,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        cpu_profile_add(GEGPU_CPU_SNAPSHOT_REGION, region_started);
        if (!cmd_batch_record(SR_PERF_GE_SNAPSHOT_COPY)) return 0;
        s_replay_stats.snapshot_copies++;
        if (s_cpu_profile) s_cpu_profile_stats.snapshot_copies++;
        sr_perf_ge_event(SR_PERF_GE_SNAPSHOT_COPIED, 1);
    }
    uint64_t metadata_started = cpu_profile_now();
    s_snap_src = src;
    s_snap_srcgen = src->render_gen;
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
        case 6:   /* 2*src alpha: exact on the src side (shader premultiplies rgb) */
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
        /* Outside clear mode PSP alpha is the stencil plane.  With stencil states routed
         * to ge.c's exact compositor, ordinary draws must preserve destination alpha. */
        k->cmask = (mr >= 0x80 ? 0 : VK_COLOR_COMPONENT_R_BIT) | (mg >= 0x80 ? 0 : VK_COLOR_COMPONENT_G_BIT)
                 | (mb >= 0x80 ? 0 : VK_COLOR_COMPONENT_B_BIT);
    }

    /* blending: fixed-function VK when it maps exactly, shader blend otherwise */
    int shblend = 0;
    b->bconst[3] = 1.0f;
    if (!clear && g->blend_enable) {
        uint32_t sfp = g->blend_mode & 0xF, dfp = (g->blend_mode >> 4) & 0xF;
        uint32_t eq = (g->blend_mode >> 8) & 7;
        /* States fixed-function Vulkan cannot express exactly:
         *  - any blend onto a 16-bit framebuffer, and dither+blend: the PSP quantizes/
         *    dithers the blend RESULT on store; the shader must see the final color
         *  - absdiff (eq 5): |src - dst| needs both subtraction orders
         *  - doubled DST-alpha factors (8/9), and doubled SRC-alpha on the DST side
         *    (6/7): no VK factor; the premultiply trick only works on the src side
         *  - two DISTINCT FIX constants: VK has a single blend-constant register
         * min/max (eq 3/4) are exact in fixed function: both PSP and VK ignore the
         * factors for them. */
        int dual_fix = sfp >= 10 && dfp >= 10 &&
                       ((g->blend_fixa ^ g->blend_fixb) & 0xFFFFFFu) != 0;
        shblend = (g->fbfmt & 3) != 3 || (g->dither_enable != 0) || eq == 5 ||
                  sfp == 8 || sfp == 9 || (dfp >= 6 && dfp <= 9) || dual_fix;
        if (shblend) {
            sr_perf_ge_event(SR_PERF_GE_SHBLEND_STATE, 1);
            if ((g->fbfmt & 3) != 3) sr_perf_ge_event(SR_PERF_GE_SHBLEND_FB16, 1);
            if (g->dither_enable != 0) sr_perf_ge_event(SR_PERF_GE_SHBLEND_DITHER, 1);
            if (eq == 5) sr_perf_ge_event(SR_PERF_GE_SHBLEND_ABSDIFF, 1);
            if (sfp == 8 || sfp == 9) sr_perf_ge_event(SR_PERF_GE_SHBLEND_DOUBLE_DST_ALPHA, 1);
            if (dfp >= 6 && dfp <= 9)
                sr_perf_ge_event(SR_PERF_GE_SHBLEND_DOUBLE_SRC_ALPHA_DST, 1);
            if (dual_fix) sr_perf_ge_event(SR_PERF_GE_SHBLEND_DUAL_FIX, 1);
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
    int flags = premul;
    if (clear) flags |= F_CLEAR;
    if (persp && !(clear && sprite)) flags |= F_PERSP;   /* minz/maxz discard */
    if (persp && g->fog_enable && !clear) flags |= F_FOG;
    b->pc.texsize[0] = b->pc.texsize[1] = 1.0f;
    if (textured) {
        flags |= F_TEX;
        if (g->tex_func & 0x100)   flags |= F_RGBA;
        if (g->tex_func & 0x10000) flags |= F_DBL;
        int linear = (g->tex_filter & 1) || ((g->tex_filter >> 8) & 1);
        if (!linear) flags |= F_NEAREST;
        b->pc.texsize[0] = (float)g->tex_w;
        b->pc.texsize[1] = (float)g->tex_h;
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

    b->pc.cfg[1] = (!clear && g->atest_enable) ? (int32_t)g->atest : 1 /* func ALWAYS */;
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
            int linear = (g->tex_filter & 1) || ((g->tex_filter >> 8) & 1);
            uint32_t bpp_s = src->fmt == 3 ? 4 : 2;
            uint32_t toff = vram_off(g->tex_addr) - src->fba;
            s_cnt_rtt++;
            /* GPU-direct: bind the target image, addressing any pixel-aligned sub-rect
             * through the texel offset in texsize.zw — no readback, no CPU decode.
             * tex_fmt must EQUAL the target's psm: reinterpreting one 16-bit format as
             * another scrambles channels/alpha and must go through VRAM pack/unpack.
             * SR_GPU_NORTT=1 forces the readback path for all draws (debug bisect). */
            if (!nortt && g->tex_fmt == src->fmt && !g->tex_swizzle &&
                (g->tex_bufw == src->stride || g->tex_bufw == 0) && (toff % bpp_s) == 0) {
                uint32_t pix = toff / bpp_s;
                if (src == s_cur) {
                    /* feedback loop: sample a snapshot copy; reuse the previous snapshot
                     * while nothing new has rendered into the target */
                    snapshot_refresh(src);
                    b->dset = linear ? s_snap_l : s_snap_n;
                } else {
                    if (s_nbatch && src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                        submit_pending();
                    if (src->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && cmd_begin()) {
                        to_layout(s_cmd, src->img, VK_IMAGE_ASPECT_COLOR_BIT, &src->layout,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                        cmd_submit_wait(SR_PERF_GE_TARGET_READBACK_TRANSITION);
                    }
                    b->dset = linear ? src->set_l : src->set_n;
                }
                /* texel coords map 1:1 onto the 512x272 image, shifted by the sub-rect */
                b->pc.texsize[0] = (float)FB_W;
                b->pc.texsize[1] = (float)FB_H;
                b->pc.texsize[2] = (float)(pix % src->stride);
                b->pc.texsize[3] = (float)(pix / src->stride);
            } else {
                /* incompatible stride/format reinterpretation: VRAM + decoder (rare) */
                target_readback(src);
                b->dset = tex_get();
            }
        } else {
            b->dset = tex_get();
        }
    }
    /* Shader blend reads the destination through the snapshot: refresh it LAST so it
     * reflects every batch submitted while building this state. Draws batched under
     * this one state then blend against that snapshot (intra-batch overlap reads the
     * pre-batch destination — the documented shader-blend hazard). */
    if (shblend) snapshot_refresh(s_cur);
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
    uint32_t clut_fmt, clut_gen;
    int atest_enable; uint32_t atest;
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
    o->clut_fmt = g->clut_fmt; o->clut_gen = g->clut_gen;
    o->atest_enable = g->atest_enable; o->atest = g->atest;
    o->fog_enable = g->fog_enable; o->fog_color = g->fog_color;
    o->fbfmt = g->fbfmt;
    o->dither_enable = g->dither_enable;
    memcpy(o->dith, g->dith, sizeof(o->dith));
    o->persp = persp; o->sprite = sprite;
    o->flushgen = s_flushgen;
}

static void state_get(int persp, int sprite, Batch *out) {
    uint64_t prep_started = cpu_profile_now();
    StateSnap sn; snap_fill(&sn, persp, sprite);
    if (s_tmpl_ok && !memcmp(&sn, &s_snapst, sizeof(sn))) {
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
    s_tmpl_ok = 1;
    *out = s_tmpl;
    if (s_cpu_profile) {
        /* Count one state-preparation operation per state_get(), not one per timed
         * segment on the cache-miss path. */
        cpu_profile_add_elapsed(GEGPU_CPU_STATE_PREP, SDL_GetTicksNS() - prep_started);
    }
}

static void append(Batch *b, uint32_t first, uint32_t count) {
    if (s_cpu_profile) s_cpu_profile_stats.append_calls++;
    if (b->sw <= 0 || count == 0) return;
    if (((uint32_t)b->pc.cfg[0] >> 8) & F_SHBLEND)
        sr_perf_ge_event(SR_PERF_GE_SHBLEND_DRAW, 1);
    if (s_nbatch) {
        Batch *last = &s_batch[s_nbatch - 1];
        if (s_cpu_profile) s_cpu_profile_stats.append_compare_calls++;
        if (last->first + last->count == first &&
            !memcmp(&last->key, &b->key, sizeof(b->key)) &&
            last->sx == b->sx && last->sy == b->sy && last->sw == b->sw && last->sh == b->sh &&
            !memcmp(last->bconst, b->bconst, sizeof(b->bconst)) &&
            !memcmp(&last->pc, &b->pc, sizeof(b->pc)) && last->dset == b->dset) {
            last->count += count;
            if (s_cpu_profile) s_cpu_profile_stats.append_merges++;
            return;
        }
    }
    b->first = first; b->count = count;
    s_batch[s_nbatch++] = *b;
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
static int gpu_state_supported(void) {
    if (s_ge->stencil_enable||s_ge->color_test_enable||s_ge->logic_op_enable)
        return 0;
    if (!s_ge->clear&&!native_rgb_mask_exact()) return 0;
    /* Framebuffer placements the persistent-target model cannot represent: color
     * buffers in main RAM, and strides wider than the target images. */
    if (!is_vram(s_ge->fbp)) return 0;
    if ((s_ge->fbw ? s_ge->fbw : 512u) > FB_W) return 0;
    return 1;
}

static int software_fallback_begin(int force) {
    if (!force&&gpu_state_supported()) return 0;
    if (s_nbatch&&!submit_pending()) return 1;

    Target *t=is_vram(s_ge->fbp)?target_find_by_fba(vram_off(s_ge->fbp)):NULL;
    if (t&&t->gpu_valid) {
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

static int hook_tri(const GeVtx *A, const GeVtx *B, const GeVtx *C, int persp) {
    if (!s_ready) return 0;
    cpu_profile_hook_enter();
    if (software_fallback_begin(0)) return cpu_profile_hook_leave(0);
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

static int hook_sprite(const GeVtx *p0, const GeVtx *p1, int persp) {
    if (!s_ready) return 0;
    cpu_profile_hook_enter();
    if (software_fallback_begin(0)) return cpu_profile_hook_leave(0);
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
    float uw = (u1 - u0) / (xb - xa), vw = (v1 - v0) / (yb - ya);
    float ua = u0 - 0.5f * uw, ub = u1 - 0.5f * uw;
    float va = v0 - 0.5f * vw, vb = v1 - 0.5f * vw;

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
    if (software_fallback_begin(1)) return cpu_profile_hook_leave(0);
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
    if (software_fallback_begin(1)) return cpu_profile_hook_leave(0);
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
    /* CPU HLE calls may use any PSP address alias (KSEG mirrors for RAM or the
     * mirrored 0x04xxxxxx VRAM aperture). Compare cache ranges in the same
     * physical namespace before deciding whether a texture is stale. The
     * target path below still uses vram_off(), because framebuffer targets are
     * indexed by their 2 MiB-local VRAM offset. */
    uint64_t dirty0 = (uint64_t)SR_PHYS(addr), dirty1 = dirty0 + bytes;
    for (int i = 0; i < s_tex_n; i++) {
        TexEnt *e = &s_tex[i];
        uint64_t tex0 = (uint64_t)SR_PHYS(e->addr), tex1 = tex0 + e->bytes;
        if (dirty0 < tex1 && tex0 < dirty1)
            if (e->content_valid) {
                s_cnt_tex_invalidations++;
                e->content_valid = 0;
            }
    }
    if (!is_vram(addr)) return;
    uint32_t a0 = vram_off(addr);
    for (int i = 0; i < MAX_TGT; i++) {
        Target *t = &s_tgts[i];
        if (!t->used || !t->gpu_valid) continue;
        uint32_t bpp_t = t->fmt == 3 ? 4 : 2;
        uint32_t flen = t->stride * FB_H * bpp_t;
        if (a0 < t->fba + flen && t->fba < a0 + bytes) {
            if (t == s_cur && s_nbatch && !submit_pending()) {
                fprintf(stderr, "gegpu: failed to submit target before CPU VRAM patch\n");
                t->gpu_valid = 0;
            } else if (!target_patch_vram_dirty(t, a0, bytes)) {
                fprintf(stderr, "gegpu: failed to preserve target across CPU VRAM patch\n");
                t->gpu_valid = 0;
            }
            s_cnt_dirty++;
        }
    }
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
        if (s_deps[i].used && !depth_to_cpu(&s_deps[i])) return 0;
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
    if (!s_ready) return -1;
    readback_poll();
    /* Display buffers in main RAM (movie frames, CPU-composed screens) are never
     * GPU-resident; folding them into the VRAM window could alias a real target. */
    if (!is_vram(fbaddr)) { s_cnt_present_cpu++; return -1; }
    uint32_t fba = vram_off(fbaddr);
    Target *t = target_color_acquire(fba, stride ? stride : 512u, (uint32_t)fmt, 1);
    if (!t || !t->gpu_valid || (int)t->fmt != (fmt & 3)) {
        { static int n = 0; if (n < 3 || getenv("SR_GPU_PRESENT_DBG"))
            fprintf(stderr, "GEGPU_PRESENT[%llu]: EARLY-EXIT t=%p gpu_valid=%d fmt=%d/%d\n",
                (unsigned long long)s_cnt_present_gpu + s_cnt_present_cpu, (void*)t, t ? t->gpu_valid : -1,
                t ? t->fmt : -1, fmt & 3); n++; }
        s_cnt_present_cpu++; return -1; }
    if (!target_prepare_present(t)) return -1;
    s_cnt_present_gpu++;
    stats_tick();
    /* Preserve SDL's tri-state result: 0 is a user close request, while -1 is a
     * recoverable presentation failure that gui_present may route through CPU VRAM. */
    int pr = sdl3vk_present_image_ex((void *)t->img, DRAW_W * s_scale, FB_H * s_scale);
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
    gegpu_capture_materialize,
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

    if (tc->pending_batch && s_nbatch) {
        fprintf(stderr, "gpu coherence selftest [%s]: dirty hook left batch pending\n", tc->name);
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

    if (t->clean_gen != t->render_gen) {
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

int gegpu_coherence_selftest(void) {
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
    if (!coherence_run_overlap_case()) ok = 0;
    coherence_reset_targets();
    return ok;
}

/* Source-owned regression for the explicit snapshot boundary. It first proves that
 * ordinary presentation remains asynchronous (the pre-fix lag), then asks the same
 * target through gegpu_sync_guest_fb() and verifies that guest VRAM is current before
 * publication. Descriptor rejection and no-target answers are checked too. */
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
    coherence_reset_targets();
    return 1;
}
#endif

int gegpu_init(void) {
    Sdl3VkInfo vi;
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
        const char *sc = getenv("SR_GPU_SCALE");
        if (sc && sc[0]) {
            s_scale = atoi(sc);
            if (s_scale < 1) s_scale = 1;
            if (s_scale > MAX_SCALE) s_scale = MAX_SCALE;
        }
        if (s_scale > 1)
            fprintf(stderr, "gegpu: render scale %dx (%ux%u internal)\n", s_scale, SCL_W, SCL_H);
    }

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(s_pdev, &props);
    s_xfer_align = props.limits.optimalBufferCopyOffsetAlignment;
    if (s_xfer_align < 4) s_xfer_align = 4;

    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = vi.queue_family;
    VKC(vkCreateCommandPool(s_dev, &cpi, NULL, &s_pool));
    VkCommandBufferAllocateInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = s_pool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
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
    VkRenderPassCreateInfo rpc = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
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
    VkDescriptorSetLayoutCreateInfo dlc = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    dlc.bindingCount = 2; dlc.pBindings = db;
    VKC(vkCreateDescriptorSetLayout(s_dev, &dlc, NULL, &s_dlayout));

    VkDescriptorPoolSize dps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * MAX_TEX };
    VkDescriptorPoolCreateInfo dpc = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    dpc.maxSets = MAX_TEX; dpc.poolSizeCount = 1; dpc.pPoolSizes = &dps;
    dpc.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;   /* LRU eviction */
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_dpool_tex));
    VkDescriptorPoolSize dpsf = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * (2 * MAX_TGT + 3) };
    dpc.maxSets = 2 * MAX_TGT + 3; dpc.pPoolSizes = &dpsf;
    dpc.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    VKC(vkCreateDescriptorPool(s_dev, &dpc, NULL, &s_dpool_fix));

    VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(PushPC) };
    VkPipelineLayoutCreateInfo plc = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    plc.setLayoutCount = 1; plc.pSetLayouts = &s_dlayout;
    plc.pushConstantRangeCount = 1; plc.pPushConstantRanges = &pcr;
    VKC(vkCreatePipelineLayout(s_dev, &plc, NULL, &s_playout));

    VkShaderModuleCreateInfo smc = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    smc.codeSize = sizeof(k_vert_spv); smc.pCode = k_vert_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_vs));
    smc.codeSize = sizeof(k_frag_spv); smc.pCode = k_frag_spv;
    VKC(vkCreateShaderModule(s_dev, &smc, NULL, &s_fs));

    /* shared clamp samplers (render-target sampling + snapshot) */
    {
        VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        sci.magFilter = VK_FILTER_NEAREST; sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VKC(vkCreateSampler(s_dev, &sci, NULL, &s_smp_n));
        sci.magFilter = VK_FILTER_LINEAR; sci.minFilter = VK_FILTER_LINEAR;
        VKC(vkCreateSampler(s_dev, &sci, NULL, &s_smp_l));
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
    s_snap_n = make_descriptor(s_snap_view, s_smp_n, s_dpool_fix);
    s_snap_l = make_descriptor(s_snap_view, s_smp_l, s_dpool_fix);
    if (!s_snap_n || !s_snap_l) return 0;

    /* 1x1 white texture for untextured draws */
    {
        uint32_t white = 0xFFFFFFFFu;
        if (!tex_make(&white, 1, 1, 0, 1, 1, &s_white, &s_white_mem, &s_white_view, &s_white_smp))
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
        if (e->view) vkDestroyImageView(s_dev, e->view, NULL);
        if (e->img) vkDestroyImage(s_dev, e->img, NULL);
        if (e->mem) vkFreeMemory(s_dev, e->mem, NULL);
    }
    s_tex_n = 0;
    for (int i = 0; i < s_pipe_n; i++)
        if (s_pipes[i].pipe) vkDestroyPipeline(s_dev, s_pipes[i].pipe, NULL);
    s_pipe_n = 0;

    if (s_white_set) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &s_white_set);
    if (s_snap_n) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &s_snap_n);
    if (s_snap_l) vkFreeDescriptorSets(s_dev, s_dpool_fix, 1, &s_snap_l);
    if (s_dpool_tex) vkDestroyDescriptorPool(s_dev, s_dpool_tex, NULL);
    if (s_dpool_fix) vkDestroyDescriptorPool(s_dev, s_dpool_fix, NULL);

    if (s_white_smp) vkDestroySampler(s_dev, s_white_smp, NULL);
    if (s_white_view) vkDestroyImageView(s_dev, s_white_view, NULL);
    if (s_white) vkDestroyImage(s_dev, s_white, NULL);
    if (s_white_mem) vkFreeMemory(s_dev, s_white_mem, NULL);
    if (s_snap_view) vkDestroyImageView(s_dev, s_snap_view, NULL);
    if (s_snapimg) vkDestroyImage(s_dev, s_snapimg, NULL);
    if (s_snapimg_mem) vkFreeMemory(s_dev, s_snapimg_mem, NULL);
    if (s_smp_n) vkDestroySampler(s_dev, s_smp_n, NULL);
    if (s_smp_l) vkDestroySampler(s_dev, s_smp_l, NULL);

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
