// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
//
/*
 * GeState/GeVtx were moved here verbatim from ge.c so the optional GPU rasterizer
 * (src/rt/gpu_sdl3vk/ge_gpu.c) can read the live GE state without duplicating it.
 * ge.c remains the only writer of the state and the arbiter of GE semantics; the GPU
 * backend registers hooks at runtime (ge_set_gpu_hooks) and ge.c stays byte-identical
 * in behaviour when no hooks are registered (the GDI build never registers any).
 */
#ifndef GE_SHARED_H
#define GE_SHARED_H

#include <stdint.h>

typedef struct {
    uint32_t base, offset;    /* GE_BASE raw data and GE_OFFSETADDR << 8 */
    uint32_t fbp, fbw, fbfmt; /* framebuffer ptr/stride/format (0=5650,1=5551,2=4444,3=8888) */
    uint32_t zbp, zbw;
    uint32_t vaddr, iaddr;
    uint32_t vtype;           /* vertex type bits */
    int scis_x1, scis_y1, scis_x2, scis_y2;
    int ztest_enable, zwrite_disable, cull_enable;
    uint32_t ztest, cull, minz, maxz;
    /* GE_CLIPENABLE (0x1C): PPSSPP's "depth clip" flag. OFF (common case): any vertex whose
     * screen x/y/z falls outside the 4096x4096x65536 grid, or whose z/w passes +-1, drops the
     * WHOLE triangle. ON: z clamps instead and only all-3-outside-same-side drops. */
    uint32_t depth_clip;
    /* Matrices: column-major. World/View are 4×3 (12 floats), Proj is 4×4 (16 floats).
     * *_num is the auto-increment write cursor set by *MATRIXNUMBER commands. */
    float world[12], view[12], proj[16];
    int world_num, view_num, proj_num;
    float bone[96];                          /* 8 bone matrices, 4x3 each (skinning) */
    int bone_num;
    float morph_weight[8];                   /* GE_MORPHWEIGHT0..7 */
    /* Viewport: raw 24-bit float mantissa words from VIEWPORTXSCALE..VIEWPORTZCENTER. */
    uint32_t viewport_raw[6];   /* [0]=XS [1]=YS [2]=ZS [3]=XC [4]=YC [5]=ZC */
    uint32_t offsetx, offsety;  /* sub-pixel offset (pixels = (raw&0xFFFF)/16.0) */
    uint32_t material;        /* flat material/ambient colour (0x00BBGGRR) */
    int material_alpha, material_alpha_set, material_set;
    int tex_enable, tex_swizzle, clear;
    uint32_t clear_mode;
    int blend_enable;
    uint32_t blend_mode, blend_fixa, blend_fixb;
    /* Fragment tests. PSP stencil is stored in the destination alpha channel; the
     * three stencil ops are packed as sfail | zfail<<8 | zpass<<16. */
    int stencil_enable; uint32_t stencil_test, stencil_op;
    int color_test_enable; uint32_t color_test, color_ref, color_test_mask;
    int logic_op_enable; uint32_t logic_op;
    uint32_t tex_addr, tex_bufw, tex_fmt;   /* level-0 texture base / stride(texels) / PSM */
    uint32_t tex_wrap, tex_func, tex_env, tex_filter;
    int tex_w, tex_h;                        /* texture dimensions (from TEXSIZE log2) */
    uint32_t clut_addr, clut_fmt;            /* palette base and format word */
    /* Internal CLUT RAM, captured at GE_LOADCLUT like the hardware. Games reuse the staging
     * buffer right after LOADCLUT, so sampling the palette live from memory reads garbage. */
    uint8_t clutram[2048];
    /* Texture coordinate transform (transform-mode only; through-mode UV are raw texels). */
    float tex_scale_u, tex_scale_v, tex_off_u, tex_off_v;
    uint32_t tex_map_mode;                   /* bits0-1: 0=UV 1=tex matrix 2=envmap; bits8-9: proj source */
    uint32_t tex_shade_ls;                   /* envmap: light indices (bits0-1, 8-9) */
    float tgen[12];                          /* texgen matrix, 4x3 column-major like world/view */
    int tgen_num;
    /* Alpha test: atest = func | ref<<8 | mask<<16 (funcs match ztest_px order). */
    int atest_enable; uint32_t atest;
    /* Fog: colour = mix(fog_color, frag, clamp01((view_z + fog_end) * fog_slope)). */
    int fog_enable; float fog_end, fog_slope; uint32_t fog_color;
    /* Shading: 1 = gouraud (default), 0 = flat (provoking vertex = last). */
    int shade_gouraud;
    /* Lighting (transform-mode, per-vertex). Lights are in world space. */
    int lighting_enable, light_on[4], reverse_normal, light_mode;
    uint32_t light_type[4];                  /* bits0-1 comp, bits8-9 type */
    float light_pos[4][3], light_dir[4][3], light_att[4][3];
    float light_spot_exp[4], light_spot_cut[4];
    uint32_t light_acol[4], light_dcol[4], light_scol[4];
    uint32_t amb_color; int amb_alpha;
    uint32_t mat_emissive, mat_diffuse, mat_specular, mat_update;
    float spec_coef;
    /* Dither: 4x4 signed offsets applied to RGB before packing to 16-bit formats. */
    int dither_enable; int8_t dith[4][4];
    /* Patch tessellation state. */
    uint32_t patch_division, patch_primitive, patch_facing;
    int patch_cull_enable;
    /* Write masks: bits set = write-disabled (per PSP semantics). */
    uint32_t mask_rgb; int mask_alpha;
    /* Bumped on every GE_LOADCLUT: lets the GPU backend detect palette changes without
     * hashing clutram per draw. */
    uint32_t clut_gen;
    /* Block transfer (GE_TRANSFER*): raw command words; decoded at GE_TRANSFERSTART with
     * PPSSPP's accessor masks. The GPU backend reads these on flush("xfersrc") to know
     * which guest range is about to be read. */
    uint32_t xf_src, xf_srcw, xf_dst, xf_dstw, xf_spos, xf_dpos, xf_size;
} GeState;

/* A rasterized vertex: screen position, reciprocal clip-w (for persp-correct interp), UV,
 * colour, and per-vertex fog factor (1 = no fog). In transform mode u/v are stored
 * pre-multiplied by rw (texel coords over w). */
typedef struct GeVtx { float x, y, z, rw; float u, v; float fog; int r, g, b, a; } GeVtx;

typedef enum GeCpuPhase {
    GE_CPU_LIST_TOTAL = 0,       /* inclusive; remaining phases partition this total */
    GE_CPU_COMMAND_DISPATCH,
    GE_CPU_PRIMITIVE,        /* inclusive of GE_CPU_GPU_HOOK */
    GE_CPU_GPU_HOOK,
    GE_CPU_BLOCK_TRANSFER,
    GE_CPU_CLUT_LOAD,
    GE_CPU_FLUSH,
    GE_CPU_PHASE_COUNT,
} GeCpuPhase;

/* Sparse, opt-in primitive-front-end diagnostics.  Vertex phases are sampled at
 * one rotating phase per sparse vertex event; clipping/assembly are sampled at
 * one rotating phase per sparse triangle candidate.  The counters are kept
 * separate from the inclusive GE CPU hierarchy so enabling this diagnostic does
 * not silently turn on the coarse list timers. */
typedef enum GePrimitiveProfilePhase {
    GE_PRIM_PROFILE_VERTEX_FETCH_DECODE = 0,
    GE_PRIM_PROFILE_TRANSFORM,
    GE_PRIM_PROFILE_LIGHTING,
    GE_PRIM_PROFILE_CLIPPING_ACCEPTANCE,
    GE_PRIM_PROFILE_ASSEMBLY,
    GE_PRIM_PROFILE_PHASE_COUNT,
} GePrimitiveProfilePhase;

typedef struct GeCpuPhaseStats {
    uint64_t calls;
    uint64_t ns;
} GeCpuPhaseStats;

typedef struct GeCpuProfileStats {
    int enabled;
    int primitive_profile_enabled;
    int primitive_profile_calibration_enabled;
    GeCpuPhaseStats phase[GE_CPU_PHASE_COUNT];
    uint64_t commands;
    uint64_t primitive_commands;
    uint64_t primitive_vertices;
    uint64_t vertex_decode_uses;
    uint64_t strip_vertices_reused;
    uint64_t primitive_type[8];
    uint64_t block_transfers;
    uint64_t clut_loads;
    uint64_t flushes;

    /* Sparse primitive-front-end profile.  `eligible` is the number of events
     * that could contribute to a phase and `calls` is the number actually
     * timed.  The replay reporter scales the raw samples by that ratio. */
    uint32_t primitive_profile_stride;
    uint64_t primitive_profile_timer_pair_ns;
    uint64_t primitive_profile_vertices;
    uint64_t primitive_profile_transform_vertices;
    uint64_t primitive_profile_triangle_candidates;
    uint64_t primitive_profile_commands[8];
    uint64_t primitive_profile_submitted[8];
    uint64_t primitive_profile_vertex_references;
    uint64_t primitive_profile_triangle_vertex_references;
    uint64_t primitive_profile_non_triangle_vertex_references;
    uint64_t primitive_profile_triangle_vertex_uses;
    uint64_t primitive_profile_non_triangle_vertex_uses;
    uint64_t primitive_profile_through_vertex_uses;
    uint64_t primitive_profile_transform_vertex_uses;
    uint64_t primitive_profile_actual_decoded_vertices;
    uint64_t primitive_profile_actual_transformed_vertices;
    uint64_t primitive_profile_actual_through_vertices;
    uint64_t primitive_profile_strip_cache_commands;
    uint64_t primitive_profile_strip_cache_hits;
    uint64_t primitive_profile_through_triangle_candidates;
    uint64_t primitive_profile_transform_triangle_candidates;
    uint64_t primitive_profile_transform_triangles_drawn;
    uint64_t primitive_profile_transform_triangles_clipped;
    uint64_t primitive_profile_transform_triangles_rejected;
    uint64_t primitive_profile_non_triangle_primitives;
    uint64_t primitive_profile_vertex_rejects;
    uint64_t primitive_profile_patch_commands;
    uint64_t primitive_profile_patch_control_vertices;
    uint64_t primitive_profile_eligible[GE_PRIM_PROFILE_PHASE_COUNT];
    GeCpuPhaseStats primitive_profile_phase[GE_PRIM_PROFILE_PHASE_COUNT];
    GeCpuPhaseStats primitive_profile_sampled_total;
    GeCpuPhaseStats primitive_profile_empty_control;
} GeCpuProfileStats;

/* GPU rasterizer hooks. The primitive hooks return 1 when the primitive was taken by
 * the GPU backend (the software rasterizer is skipped) — in the full GPU renderer they
 * always take it. flush(reason) is called at GE sync points ("listend", "loadclut");
 * the backend decides what each requires. vram_dirty(addr,bytes) is a POST-WRITE notification:
 * guest memory already contains the CPU's new bytes. An overlapping persistent target must
 * merge exactly that byte span, preserve newer GPU bytes outside it, and prevent an older
 * asynchronous readback from committing over the CPU generation. */
typedef struct GeGpuHooks {
    int  (*tri)(const GeVtx *a, const GeVtx *b, const GeVtx *c, int persp);
    int  (*sprite)(const GeVtx *p0, const GeVtx *p1, int persp);
    int  (*line)(const GeVtx *a, const GeVtx *b, int persp);
    int  (*point)(const GeVtx *a, int persp);
    void (*flush)(const char *reason);
    void (*vram_dirty)(uint32_t addr, uint32_t bytes);
    /* GE block transfer (state in xf_*): returns 1 when the copy was performed GPU-side
     * (image-to-image; guest VRAM intentionally left stale, like any render target). 0 =
     * not representable (RAM endpoints, format reinterpretation): caller runs the CPU copy,
     * preceded by flush("xfersrc") and followed by vram_dirty on the destination. Games run
     * this blit per frame (motion blur, pause capture); the CPU route costs a GPU drain +
     * PCIe readback + re-upload each time, which dwarfs the frame itself on discrete GPUs. */
    int  (*xfer)(uint32_t startdata);
    /* Diagnostic capture boundary: materialize persistent color/depth images into the
     * CPU mirrors before a standalone fixture snapshots its exact initial state. */
    int  (*capture_boundary)(void);
} GeGpuHooks;

/* Implemented in ge.c. */
void      ge_set_gpu_hooks(const GeGpuHooks *h);
/* Post-write notification for CPU-side writes that may alias GPU-cached framebuffers
 * (movie decode, DMA memcpy). No-op when no GPU backend is registered. */
void      sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
GeState  *ge_state_ptr(void);
uint16_t *ge_zbuf_ptr(void);        /* 272*512 entries, row stride = ge_zbuf_stride() */
uint32_t  ge_zbuf_stride(void);
/* Decode the CURRENT texture (ge.tex_addr, tex_w x tex_h) to RGBA8 through the exact
 * sampler path the software rasterizer uses (swizzle + CLUT + format). out must hold
 * tex_w*tex_h words; layout is row-major, r|g<<8|b<<16|a<<24. */
void      ge_decode_tex_rgba(uint32_t *out);
void      ge_replay_restore(const GeState *state, const uint16_t *zbuf);
void      ge_cpu_profile_reset(void);
void      ge_cpu_profile_get(GeCpuProfileStats *out);

#endif /* GE_SHARED_H */
