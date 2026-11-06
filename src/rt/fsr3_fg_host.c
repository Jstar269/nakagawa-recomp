/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * Derived from the AMD FidelityFX SDK v2.4.0 (commit 7539ae8b6568ad29ed78a753404351daf3b5cca7), under the MIT
 * licence below, from these files under Kits/FidelityFX/:
 *   framegeneration/fsr3/internal/ffx_frameinterpolation.cpp (frame interpolation: creation, prepare, the
 *     dispatch plan, constants, clears, discards, barriers and the copy);
 *   framegeneration/fsr3/internal/ffx_frameinterpolation_private.h (its constant-buffer layouts);
 *   framegeneration/fsr3/include/ffx_frameinterpolation.h (its creation flags);
 *   framegeneration/fsr3/internal/ffx_opticalflow.cpp (optical flow: creation, the dispatch plan and its
 *     pyramid loop, constants, clears and barriers);
 *   framegeneration/fsr3/internal/ffx_opticalflow_private.h (its binding identifiers and constant buffer);
 *   framegeneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp (the provider: which frame's
 *     prepared inputs a dispatch uses, its reset, and what it passes to optical flow and interpolation);
 *   framegeneration/fsr3/include/gpu/spd/ffx_spd.h (ffxSpdSetup with a mip count).
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files(the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions :
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 * Modified by Nakagawa Recomp contributors, 2026-11-05: translated from AMD's C++ host to C.
 */
/* fsr3_fg_host.c - see fsr3_fg_host.h. Comments name the AMD function each part translates. */
#include "fsr3_fg_host.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define FFX_CPU 1
#include "frameinterpolation/ffx_frameinterpolation_resources.h"
#include "opticalflow/ffx_opticalflow_resources.h"

typedef char fsr3fg_constants_size_is_amds[sizeof(Fsr3FgConstants) == 192 ? 1 : -1];
typedef char fsr3fg_of_constants_size_is_amds[sizeof(Fsr3OfConstants) == 32 ? 1 : -1];
typedef char fsr3fg_of_spd_size_is_amds[sizeof(Fsr3OfSpdConstants) == 32 ? 1 : -1];

#define FI(x) FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_##x
#define OFR(x) (FSR3FG_RES_OF + FFX_OF_RESOURCE_IDENTIFIER_##x)

/* OpticalFlowBindingIdentifiers (ffx_opticalflow_private.h). */
enum {
    OFB_NULL = 0,
    OFB_INPUT_COLOR,
    OFB_OPTICAL_FLOW_INPUT,
    OFB_OPTICAL_FLOW_INPUT_LEVEL_1,
    OFB_OPTICAL_FLOW_INPUT_LEVEL_2,
    OFB_OPTICAL_FLOW_INPUT_LEVEL_3,
    OFB_OPTICAL_FLOW_INPUT_LEVEL_4,
    OFB_OPTICAL_FLOW_INPUT_LEVEL_5,
    OFB_OPTICAL_FLOW_INPUT_LEVEL_6,
    OFB_OPTICAL_FLOW_PREVIOUS_INPUT,
    OFB_OPTICAL_FLOW_SCD_HISTOGRAM,
    OFB_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM,
    OFB_OPTICAL_FLOW_SCD_TEMP,
    OFB_OPTICAL_FLOW_SCD_OUTPUT,
    OFB_OPTICAL_FLOW,
    OFB_OPTICAL_FLOW_NEXT_LEVEL,
    OFB_OPTICAL_FLOW_PREVIOUS,
    OFB_OPTICAL_FLOW_ALIAS_LEVEL_1,
    OFB_OPTICAL_FLOW_ALIAS_LEVEL_2,
    OFB_OPTICAL_FLOW_ALIAS_LEVEL_3,
    OFB_OPTICAL_FLOW_ALIAS_LEVEL_4,
    OFB_OPTICAL_FLOW_ALIAS_LEVEL_5,
    OFB_OPTICAL_FLOW_ALIAS_LEVEL_6,
    OFB_SHARED_OPTICAL_FLOW_VECTOR,
    OFB_SHARED_OPTICAL_FLOW_SCD_OUTPUT,
    OFB_COUNT
};
typedef char fsr3fg_of_binding_count[OFB_COUNT == FSR3FG_OF_BINDING_COUNT ? 1 : -1];

/* FfxApiResourceStates and FfxBarrierType values the plan uses. */
#define STATE_UNORDERED_ACCESS 2u
#define STATE_COPY_DEST 32u

#define PROVIDER_MAX_QUEUED_FRAMES 2u /* ffx_provider_fsr3framegeneration.cpp MAX_QUEUED_FRAMES */
#define OF_MAX_QUEUED_FRAMES 16u      /* FFX_OPTICALFLOW_MAX_QUEUED_FRAMES; even, so parity alternates */
#define OF_PYRAMID_LEVELS 7           /* advancedAlgorithmIterations, OpticalFlowMaxPyramidLevels */
#define OF_BLOCK_SIZE 8u              /* the provider's opticalFlowBlockSize and optical flow's minBlockSize */
#define OF_HISTOGRAMS_PER_DIM 3u
#define OF_HISTOGRAM_SHIFTS 3u

/* --- ffxSpdSetup, GetOpticalFlowTextureSize --------------------------------------------------- */

void fsr3_fg_spd_setup(uint32_t dispatch_groups[2], uint32_t work_group_offset[2],
                       uint32_t num_work_groups_and_mips[2], const uint32_t rect[4], int32_t mips) {
    work_group_offset[0] = rect[0] / 64;
    work_group_offset[1] = rect[1] / 64;
    const uint32_t end_x = (rect[0] + rect[2] - 1) / 64;
    const uint32_t end_y = (rect[1] + rect[3] - 1) / 64;
    dispatch_groups[0] = end_x + 1 - work_group_offset[0];
    dispatch_groups[1] = end_y + 1 - work_group_offset[1];
    num_work_groups_and_mips[0] = dispatch_groups[0] * dispatch_groups[1];
    if (mips >= 0) {
        num_work_groups_and_mips[1] = (uint32_t)mips;
    } else {
        /* From the larger side, floor(log2(resolution)) capped at 12. */
        const uint32_t resolution = rect[2] > rect[3] ? rect[2] : rect[3];
        const float m = floorf(log2f((float)resolution));
        num_work_groups_and_mips[1] = (uint32_t)(m < 12.0f ? m : 12.0f);
    }
}

void fsr3_fg_of_texture_size(uint32_t display_width, uint32_t display_height, uint32_t size[2]) {
    size[0] = (display_width + OF_BLOCK_SIZE - 1) / OF_BLOCK_SIZE;
    size[1] = (display_height + OF_BLOCK_SIZE - 1) / OF_BLOCK_SIZE;
}

/* --- creation: the provider's CreateContext, opticalflowCreate, frameinterpolationCreate -------- */

int fsr3_fg_host_init(Fsr3FgHost *h, const Fsr3FgHostDesc *desc) {
    const uint32_t accepted =
        FSR3FG_HOST_ENABLE_DEPTH_INVERTED | FSR3FG_HOST_ENABLE_DEPTH_INFINITE | FSR3FG_HOST_ENABLE_HDR_COLOR_INPUT;
    memset(h, 0, sizeof(*h));
    if ((desc->flags & ~accepted) || !(desc->flags & FSR3FG_HOST_ENABLE_DEPTH_INVERTED)) return -1;
    if (!desc->max_render_width || !desc->max_render_height || !desc->display_width || !desc->display_height)
        return -1;
    h->desc = *desc;

    /* opticalflowCreate: every internal image under its own id; no binding set yet. */
    h->of_first_execution = 1;
    h->of_resource_frame_index = 0;
    h->of.inputLumaResolution[0] = (int32_t)desc->display_width;
    h->of.inputLumaResolution[1] = (int32_t)desc->display_height;

    /* frameinterpolationCreate */
    Fsr3FgConstants *c = &h->fi;
    c->maxRenderSize[0] = (int32_t)desc->max_render_width;
    c->maxRenderSize[1] = (int32_t)desc->max_render_height;
    c->displaySize[0] = (int32_t)desc->display_width;
    c->displaySize[1] = (int32_t)desc->display_height;
    c->displaySizeRcp[0] = 1.0f / (float)desc->display_width;
    c->displaySizeRcp[1] = 1.0f / (float)desc->display_height;
    c->interpolationRectSize[0] = (int32_t)desc->display_width;
    c->interpolationRectSize[1] = (int32_t)desc->display_height;
    c->colorRectSize[0] = (int32_t)desc->display_width;
    c->colorRectSize[1] = (int32_t)desc->display_height;
    /* The created resources fill srvResources under their own ids, and uavResources is a copy. */
    static const uint32_t created[] = {
        FI(RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME), FI(GAME_MOTION_VECTOR_FIELD_X), FI(GAME_MOTION_VECTOR_FIELD_Y),
        FI(INPAINTING_PYRAMID), FI(COUNTERS), FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_X),
        FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y), FI(PREVIOUS_INTERPOLATION_SOURCE), FI(INPAINTING_MASK),
        FI(DISOCCLUSION_MASK), FI(DEFAULT_DISTORTION_FIELD),
    };
    for (size_t i = 0; i < sizeof(created) / sizeof(created[0]); i++)
        h->fi_srv[created[i]] = h->fi_uav[created[i]] = created[i];
    return 0;
}

void fsr3_fg_host_configure(Fsr3FgHost *h, uint64_t frame_id, int has_hudless) {
    h->has_hudless = has_hudless != 0;
    h->last_configure_frame_id = frame_id;
}

/* --- the plan -------------------------------------------------------------------------------- */

static void plan_begin(Fsr3FgHost *h) {
    h->job_count = 0;
    h->stage_count = 0;
    h->overflow = 0;
    for (int k = 0; k < 4; k++) h->cb_stage[k] = UINT32_MAX;
}

static Fsr3FgJob *job_new(Fsr3FgHost *h, Fsr3FgJobKind kind) {
    if (h->job_count == FSR3FG_MAX_JOBS) {
        h->overflow = 1;
        return NULL;
    }
    Fsr3FgJob *j = &h->jobs[h->job_count++];
    memset(j, 0, sizeof(*j));
    j->kind = kind;
    return j;
}

/* fpStageConstantBufferDataFunc */
static void stage(Fsr3FgHost *h, Fsr3FgStageKind kind, const void *data, uint32_t bytes) {
    if (h->stage_count == FSR3FG_MAX_STAGES || bytes > sizeof(h->stages[0].words)) {
        h->overflow = 1;
        return;
    }
    Fsr3FgStage *s = &h->stages[h->stage_count];
    s->kind = kind;
    s->count = bytes / 4;
    memset(s->words, 0, sizeof(s->words));
    memcpy(s->words, data, bytes);
    h->cb_stage[kind] = h->stage_count;
    Fsr3FgJob *j = job_new(h, FSR3FG_JOB_STAGE);
    if (j) j->stage = h->stage_count;
    h->stage_count++;
}

static uint32_t float_bits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

/* FFX_GPU_JOB_CLEAR_FLOAT: the colour as AMD's four 32-bit words (floats, or the integer clears'
 * uint32 zeros copied into the same array). */
static void job_clear(Fsr3FgHost *h, uint32_t resource, uint32_t v) {
    Fsr3FgJob *j = job_new(h, FSR3FG_JOB_CLEAR);
    if (!j) return;
    j->resource = resource;
    for (int k = 0; k < 4; k++) j->value[k] = v;
}

static void job_barrier(Fsr3FgHost *h, uint32_t resource, uint32_t state) {
    Fsr3FgJob *j = job_new(h, FSR3FG_JOB_BARRIER);
    if (!j) return;
    j->resource = resource;
    j->state = state;
}

static void job_discard(Fsr3FgHost *h, uint32_t resource) {
    Fsr3FgJob *j = job_new(h, FSR3FG_JOB_DISCARD);
    if (j) j->resource = resource;
}

static void job_copy(Fsr3FgHost *h, uint32_t src, uint32_t dst) {
    Fsr3FgJob *j = job_new(h, FSR3FG_JOB_COPY);
    if (!j) return;
    j->resource = src;
    j->dest = dst;
}

/* scheduleDispatch, frame interpolation's and optical flow's: the bindings resolve through the
 * context's tables as they stand now. A constant buffer binds the latest region staged in this call. */
static void job_dispatch(Fsr3FgHost *h, Fsr3FgPassId pass, uint32_t x, uint32_t y, uint32_t z) {
    Fsr3FgJob *j = job_new(h, FSR3FG_JOB_DISPATCH);
    if (!j) return;
    const Fsr3FgPassLayout *l = &fsr3_fg_pass_layout[pass];
    j->pass = pass;
    j->groups[0] = x;
    j->groups[1] = y;
    j->groups[2] = z;
    if (l->count > FSR3FG_MAX_BINDINGS) h->overflow = 1;
    for (uint32_t i = 0; i < l->count && i < FSR3FG_MAX_BINDINGS; i++) {
        const Fsr3FgBinding *b = &l->bindings[i];
        uint32_t res = 0, mip = 0;
        switch (b->kind) {
        case FSR3FG_BIND_CBV:
            res = h->cb_stage[b->id];
            break;
        case FSR3FG_BIND_SAMPLER:
            res = b->id;
            break;
        case FSR3FG_BIND_SRV:
        case FSR3FG_BIND_SRV_BUFFER:
            res = l->optical_flow ? h->of_srv[b->id] : h->fi_srv[b->id];
            break;
        case FSR3FG_BIND_UAV:
        case FSR3FG_BIND_UAV_BUFFER:
            if (l->optical_flow) {
                res = h->of_uav[b->id];
            } else if (b->kind == FSR3FG_BIND_UAV && b->id >= FI(INPAINTING_PYRAMID_MIPMAP_0) &&
                       b->id <= FI(INPAINTING_PYRAMID_MIPMAP_12)) {
                res = h->fi_uav[FI(INPAINTING_PYRAMID)];
                mip = b->id - FI(INPAINTING_PYRAMID_MIPMAP_0);
            } else {
                res = h->fi_uav[b->id];
            }
            break;
        }
        j->res[i] = res;
        j->mip[i] = mip;
    }
}

/* --- ffxFrameInterpolationPrepare and the provider's prepare ----------------------------------- */

int fsr3_fg_host_prepare(Fsr3FgHost *h, const Fsr3FgHostPrepare *p) {
    if (!p->render_width || !p->render_height || p->render_width > h->desc.max_render_width ||
        p->render_height > h->desc.max_render_height)
        return -1;
    plan_begin(h);

    /* The provider keeps the description for the dispatch of the same frame and flips its shared set. */
    h->prepared[p->frame_id % PROVIDER_MAX_QUEUED_FRAMES] = *p;
    h->shared_toggle = (h->shared_toggle + 1) & 1u;

    Fsr3FgConstants *c = &h->fi;
    c->renderSize[0] = (int32_t)p->render_width;
    c->renderSize[1] = (int32_t)p->render_height;
    c->jitter[0] = p->jitter_x;
    c->jitter[1] = p->jitter_y;
    /* Render-size motion vectors (init refuses the display-size flag). */
    c->motionVectorScale[0] = p->motion_vector_scale_x / (float)c->renderSize[0];
    c->motionVectorScale[1] = p->motion_vector_scale_y / (float)c->renderSize[1];
    stage(h, FSR3FG_STAGE_FI, c, sizeof(*c));

    h->fi_srv[FI(DEPTH)] = FSR3FG_RES_DEPTH;
    h->fi_srv[FI(MOTION_VECTORS)] = FSR3FG_RES_MOTION_VECTORS;
    h->fi_uav[FI(DILATED_DEPTH)] = FSR3FG_RES_DILATED_DEPTH_0 + h->shared_toggle;
    h->fi_uav[FI(DILATED_MOTION_VECTORS)] = FSR3FG_RES_DILATED_MOTION_VECTORS_0 + h->shared_toggle;
    h->fi_uav[FI(RECONSTRUCTED_DEPTH_PREVIOUS_FRAME)] = FSR3FG_RES_RECONSTRUCTED_PREVIOUS_DEPTH_0 + h->shared_toggle;

    /* "Clear Reconstructed Previous Nearest Depth": 0 for inverted depth (init requires it). */
    job_clear(h, h->fi_uav[FI(RECONSTRUCTED_DEPTH_PREVIOUS_FRAME)], float_bits(0.0f));
    job_dispatch(h, FSR3FG_PASS_RECONSTRUCT_AND_DILATE, (p->render_width + 7) / 8, (p->render_height + 7) / 8, 1);

    h->fi_srv[FI(DEPTH)] = FSR3FG_RES_NULL;
    h->fi_srv[FI(MOTION_VECTORS)] = FSR3FG_RES_NULL;
    h->fi_uav[FI(DILATED_DEPTH)] = FSR3FG_RES_NULL;
    h->fi_uav[FI(DILATED_MOTION_VECTORS)] = FSR3FG_RES_NULL;
    h->fi_uav[FI(RECONSTRUCTED_DEPTH_PREVIOUS_FRAME)] = FSR3FG_RES_NULL;
    return h->overflow ? -1 : 0;
}

/* --- optical flow's dispatch ----------------------------------------------------------------- */

static void of_dispatch(Fsr3FgHost *h, uint32_t color, int reset, uint32_t transfer, const float min_max[2]) {
    uint32_t *srv = h->of_srv, *uav = h->of_uav;
    const uint32_t width = h->desc.display_width, height = h->desc.display_height;
    Fsr3OfConstants *c = &h->of;

    uav[OFB_SHARED_OPTICAL_FLOW_VECTOR] = FSR3FG_RES_OF_VECTOR;
    uav[OFB_SHARED_OPTICAL_FLOW_SCD_OUTPUT] = FSR3FG_RES_OF_SCD;
    srv[OFB_INPUT_COLOR] = color;

    c->backbufferTransferFunction = transfer;
    c->minMaxLuminance[0] = min_max[0];
    c->minMaxLuminance[1] = min_max[1];

    const int reset_accumulation = reset || h->of_first_execution;
    h->of_first_execution = 0;
    c->frameIndex = reset_accumulation ? 0 : c->frameIndex + 1;

    if (reset_accumulation) {
        job_clear(h, OFR(OPTICAL_FLOW_SCD_TEMP), 0);
        job_clear(h, uav[OFB_SHARED_OPTICAL_FLOW_SCD_OUTPUT], 0);
        job_clear(h, OFR(OPTICAL_FLOW_SCD_HISTOGRAM), 0);
        job_clear(h, OFR(OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM), 0);
        for (uint32_t k = 0; k < 7; k++) job_clear(h, OFR(OPTICAL_FLOW_INPUT_1) + k, 0);
        for (uint32_t k = 0; k < 7; k++) job_clear(h, OFR(OPTICAL_FLOW_INPUT_2) + k, 0);
    }

    uint32_t pyramid_groups[2], wg_offset[2], wg_mips[2];
    const uint32_t rect[4] = { 0, 0, width, height };
    fsr3_fg_spd_setup(pyramid_groups, wg_offset, wg_mips, rect, 4);
    Fsr3OfSpdConstants spd;
    memset(&spd, 0, sizeof(spd));
    spd.numworkGroups = wg_mips[0];
    spd.mips = wg_mips[1];
    spd.workGroupOffset[0] = wg_offset[0];
    spd.workGroupOffset[1] = wg_offset[1];
    spd.numworkGroupsOpticalFlowInputPyramid = wg_mips[0];
    stage(h, FSR3FG_STAGE_OF, c, sizeof(*c));
    stage(h, FSR3FG_STAGE_OF_SPD, &spd, sizeof(spd));

    uav[OFB_OPTICAL_FLOW_SCD_HISTOGRAM] = OFR(OPTICAL_FLOW_SCD_HISTOGRAM);
    uav[OFB_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM] = OFR(OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM);
    uav[OFB_OPTICAL_FLOW_SCD_TEMP] = OFR(OPTICAL_FLOW_SCD_TEMP);
    uav[OFB_OPTICAL_FLOW_SCD_OUTPUT] = uav[OFB_SHARED_OPTICAL_FLOW_SCD_OUTPUT];

    const int odd_frame = (h->of_resource_frame_index & 1u) != 0;
    const uint32_t input = odd_frame ? OFR(OPTICAL_FLOW_INPUT_2) : OFR(OPTICAL_FLOW_INPUT_1);
    const uint32_t previous_input = odd_frame ? OFR(OPTICAL_FLOW_INPUT_1) : OFR(OPTICAL_FLOW_INPUT_2);
    const uint32_t flow = odd_frame ? OFR(OPTICAL_FLOW_2) : OFR(OPTICAL_FLOW_1);
    const uint32_t previous_flow = odd_frame ? OFR(OPTICAL_FLOW_1) : OFR(OPTICAL_FLOW_2);
    for (uint32_t k = 0; k < 7; k++) uav[OFB_OPTICAL_FLOW_INPUT + k] = input + k;
    srv[OFB_OPTICAL_FLOW_INPUT] = input;
    srv[OFB_OPTICAL_FLOW_PREVIOUS_INPUT] = previous_input;
    srv[OFB_OPTICAL_FLOW] = flow;
    uav[OFB_OPTICAL_FLOW] = flow;
    srv[OFB_OPTICAL_FLOW_PREVIOUS] = previous_flow;

    /* Prepare luma: 16x16 groups of 2x2 pixels. */
    job_dispatch(h, FSR3FG_PASS_OF_PREPARE_LUMA, ((width + 1) / 2 + 15) / 16, ((height + 1) / 2 + 15) / 16, 1);
    job_dispatch(h, FSR3FG_PASS_OF_INPUT_PYRAMID, pyramid_groups[0], pyramid_groups[1], 1);
    {
        const uint32_t strata_width = (width / 4) / OF_HISTOGRAMS_PER_DIM;
        job_dispatch(h, FSR3FG_PASS_OF_SCD_HISTOGRAM, (strata_width + 31) / 32, 16,
                     OF_HISTOGRAMS_PER_DIM * OF_HISTOGRAMS_PER_DIM);
        job_dispatch(h, FSR3FG_PASS_OF_SCD_DIVERGENCE, OF_HISTOGRAMS_PER_DIM * OF_HISTOGRAMS_PER_DIM,
                     OF_HISTOGRAM_SHIFTS, 1);
    }

    uint32_t sizes[OF_PYRAMID_LEVELS][2];
    fsr3_fg_of_texture_size(width, height, sizes[0]);
    for (int i = 1; i < OF_PYRAMID_LEVELS; i++) {
        sizes[i][0] = (sizes[i - 1][0] + 1) / 2;
        sizes[i][1] = (sizes[i - 1][1] + 1) / 2;
    }
    for (int level = OF_PYRAMID_LEVELS - 1; level >= 0; level--) {
        const int odd_level = (level & 1) != 0;
        const uint32_t lv = (uint32_t)level;
        const uint32_t input_a = odd_frame ? OFR(OPTICAL_FLOW_INPUT_2) : OFR(OPTICAL_FLOW_INPUT_1);
        const uint32_t input_b = odd_frame ? OFR(OPTICAL_FLOW_INPUT_1) : OFR(OPTICAL_FLOW_INPUT_2);
        const uint32_t flow_a = (odd_frame != odd_level) ? OFR(OPTICAL_FLOW_2) : OFR(OPTICAL_FLOW_1);
        const uint32_t flow_b = (odd_frame != odd_level) ? OFR(OPTICAL_FLOW_1) : OFR(OPTICAL_FLOW_2);
        c->opticalFlowPyramidLevel = lv;
        c->opticalFlowPyramidLevelCount = OF_PYRAMID_LEVELS;
        stage(h, FSR3FG_STAGE_OF, c, sizeof(*c));

        srv[OFB_OPTICAL_FLOW_INPUT] = input_a + lv;
        srv[OFB_OPTICAL_FLOW_PREVIOUS_INPUT] = input_b + lv;
        uav[OFB_OPTICAL_FLOW] = flow_a + lv;
        srv[OFB_OPTICAL_FLOW_PREVIOUS] = flow_b + lv;
        {
            /* Search: 4 pixels a thread, 16 rows a group, 64 threads a group. */
            const uint32_t luma_w = (width >> lv) > 1 ? (width >> lv) : 1;
            const uint32_t luma_h = (height >> lv) > 1 ? (height >> lv) : 1;
            job_dispatch(h, FSR3FG_PASS_OF_SEARCH, ((luma_w + 3) / 4 * 16 + 63) / 64, (luma_h + 15) / 16, 1);
        }

        srv[OFB_OPTICAL_FLOW_PREVIOUS] = flow_a + lv;
        uav[OFB_OPTICAL_FLOW] = flow_b + lv;
        if (level == 0) uav[OFB_OPTICAL_FLOW] = uav[OFB_SHARED_OPTICAL_FLOW_VECTOR];
        job_dispatch(h, FSR3FG_PASS_OF_FILTER, (sizes[level][0] + 15) / 16, (sizes[level][1] + 3) / 4, 1);

        if (level > 0) {
            srv[OFB_OPTICAL_FLOW_ALIAS_LEVEL_1 + lv - 1] = uav[OFB_OPTICAL_FLOW];
            srv[OFB_OPTICAL_FLOW] = flow_b + lv;
            uav[OFB_OPTICAL_FLOW_NEXT_LEVEL] = flow_b + lv - 1;
            job_dispatch(h, FSR3FG_PASS_OF_SCALE, (sizes[level - 1][0] + 3) / 4, (sizes[level - 1][1] + 3) / 4, 1);
            job_barrier(h, srv[OFB_OPTICAL_FLOW], STATE_UNORDERED_ACCESS);
        }
        job_barrier(h, srv[OFB_OPTICAL_FLOW_PREVIOUS], STATE_UNORDERED_ACCESS);
    }

    for (uint32_t k = 0; k < 7; k++) job_barrier(h, OFR(OPTICAL_FLOW_INPUT_1) + k, STATE_UNORDERED_ACCESS);
    for (uint32_t k = 0; k < 7; k++) job_barrier(h, OFR(OPTICAL_FLOW_INPUT_2) + k, STATE_UNORDERED_ACCESS);

    h->of_resource_frame_index = (h->of_resource_frame_index + 1) % OF_MAX_QUEUED_FRAMES;
}

/* --- frame interpolation's dispatch ----------------------------------------------------------- */

static const float debug_bar_colors[7][3] = {
    { 0.0f, 1.0f, 1.0f }, { 1.0f, 0.42f, 0.0f }, { 0.0f, 0.16f, 1.0f }, { 0.74f, 1.0f, 0.0f },
    { 0.68f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.1f }, { 1.0f, 1.0f, 0.48f },
};

/* setupDeviceDepthToViewSpaceDepthParams (frame interpolation's: element e is scaled to metres). */
static void device_depth_to_view(Fsr3FgHost *h) {
    const int inverted = (h->desc.flags & FSR3FG_HOST_ENABLE_DEPTH_INVERTED) != 0;
    const int infinite = (h->desc.flags & FSR3FG_HOST_ENABLE_DEPTH_INFINITE) != 0;
    float fmin = h->rd_camera_near < h->rd_camera_far ? h->rd_camera_near : h->rd_camera_far;
    float fmax = h->rd_camera_near < h->rd_camera_far ? h->rd_camera_far : h->rd_camera_near;
    if (inverted) {
        const float t = fmin;
        fmin = fmax;
        fmax = t;
    }
    const float q = fmax / (fmin - fmax);
    const float d = -1.0f;
    const float elem_c[2][2] = { { q, -1.0f - FLT_EPSILON }, { q, 0.0f + FLT_EPSILON } };
    const float elem_e[2][2] = { { q * fmin, -fmin - FLT_EPSILON }, { q * fmin, fmax } };
    h->fi.deviceToViewDepth[0] = d * elem_c[inverted][infinite];
    h->fi.deviceToViewDepth[1] = elem_e[inverted][infinite] * h->rd_meters;
    const float aspect = (float)h->rd_render_width / (float)h->rd_render_height;
    const float cot_half_fov_y = cosf(0.5f * h->rd_camera_fov) / sinf(0.5f * h->rd_camera_fov);
    const float a = cot_half_fov_y / aspect;
    const float b = cot_half_fov_y;
    h->fi.deviceToViewDepth[2] = 1.0f / a;
    h->fi.deviceToViewDepth[3] = 1.0f / b;
}

static void inpainting_pyramid(Fsr3FgHost *h, Fsr3FgPassId pass, uint32_t width, uint32_t height) {
    uint32_t groups[2], offset[2], wg_mips[2];
    const uint32_t rect[4] = { 0, 0, width, height };
    fsr3_fg_spd_setup(groups, offset, wg_mips, rect, -1);
    h->inpainting.numworkGroups = wg_mips[0];
    h->inpainting.mips = wg_mips[1];
    h->inpainting.workGroupOffset[0] = offset[0];
    h->inpainting.workGroupOffset[1] = offset[1];
    stage(h, FSR3FG_STAGE_INPAINTING, &h->inpainting, sizeof(h->inpainting));
    job_dispatch(h, pass, groups[0], groups[1], 1);
}

typedef struct {
    uint64_t frame_id;
    int reset;
    const Fsr3FgHostPrepare *prepared;
    uint32_t rect[4];
    uint32_t transfer;
    float min_max[2];
} FiDispatch;

static void fi_dispatch(Fsr3FgHost *h, const FiDispatch *d) {
    uint32_t *srv = h->fi_srv, *uav = h->fi_uav;
    Fsr3FgConstants *c = &h->fi;
    const Fsr3FgHostPrepare *p = d->prepared;
    const uint32_t display_w = h->desc.display_width, display_h = h->desc.display_height;

    const int reset = h->fi_dispatch_count == 0 || d->reset;
    const int decreased = d->frame_id < h->fi_previous_frame_id;
    const int skipped = (d->frame_id - h->fi_previous_frame_id) > 1;
    h->fi_previous_frame_id = d->frame_id;
    h->fi_dispatch_count++;

    c->renderSize[0] = (int32_t)p->render_width;
    c->renderSize[1] = (int32_t)p->render_height;
    c->displaySize[0] = (int32_t)display_w;
    c->displaySize[1] = (int32_t)display_h;
    c->displaySizeRcp[0] = 1.0f / (float)display_w;
    c->displaySizeRcp[1] = 1.0f / (float)display_h;
    c->Mode = 0;
    c->Reset = reset || decreased || skipped;
    c->deltaTime = p->frame_time_delta;
    c->HUDLessAttachedFactor = h->has_hudless ? 1 : 0;
    /* The provider's opticalFlowScale (1 / display) and block size (8); no debug flags. */
    c->opticalFlowScale[0] = 1.0f / (float)display_w;
    c->opticalFlowScale[1] = 1.0f / (float)display_h;
    c->opticalFlowBlockSize = (int32_t)OF_BLOCK_SIZE;
    c->dispatchFlags = 0;
    c->cameraNear = p->camera_near;
    c->cameraFar = p->camera_far;
    c->interpolationRectBase[0] = (int32_t)d->rect[0];
    c->interpolationRectBase[1] = (int32_t)d->rect[1];
    c->interpolationRectSize[0] = (int32_t)d->rect[2];
    c->interpolationRectSize[1] = (int32_t)d->rect[3];
    c->colorRectBase[0] = (int32_t)d->rect[0];
    c->colorRectBase[1] = (int32_t)d->rect[1];
    c->colorRectSize[0] = (int32_t)d->rect[2];
    c->colorRectSize[1] = (int32_t)d->rect[3];
    memcpy(c->debugBarColor, debug_bar_colors[h->debug_bar_index], sizeof(c->debugBarColor));
    h->debug_bar_index = (h->debug_bar_index + 1) % 7u;
    c->backBufferTransferFunction = d->transfer;
    c->minMaxLuminance[0] = d->min_max[0];
    c->minMaxLuminance[1] = d->min_max[1];

    /* The horizontal FOV from the vertical one, with <cmath>'s float overloads as MSVC takes them. */
    const float aspect_ratio = (float)p->render_width / (float)p->render_height;
    const float camera_angle_horizontal = atanf(tanf(p->camera_fov_angle_vertical / 2.0f) * aspect_ratio) * 2.0f;
    c->fTanHalfFOV = tanf(camera_angle_horizontal * 0.5f);
    /* No distortion field: AMD's default 1x1 field. */
    c->distortionFieldSize[0] = 1;
    c->distortionFieldSize[1] = 1;

    h->rd_camera_far = p->camera_far;
    h->rd_camera_near = p->camera_near;
    h->rd_meters = p->view_space_to_meters_factor > 0.0f ? p->view_space_to_meters_factor : 1.0f;
    h->rd_camera_fov = p->camera_fov_angle_vertical;
    h->rd_render_width = p->render_width;
    h->rd_render_height = p->render_height;
    device_depth_to_view(h);
    stage(h, FSR3FG_STAGE_FI, c, sizeof(*c));

    if (c->HUDLessAttachedFactor == 1) {
        srv[FI(PRESENT_BACKBUFFER)] = FSR3FG_RES_PRESENT_COLOR;
        srv[FI(CURRENT_INTERPOLATION_SOURCE)] = FSR3FG_RES_HUDLESS_COLOR;
    } else {
        srv[FI(CURRENT_INTERPOLATION_SOURCE)] = FSR3FG_RES_PRESENT_COLOR;
        srv[FI(PRESENT_BACKBUFFER)] = srv[FI(CURRENT_INTERPOLATION_SOURCE)];
    }
    srv[FI(DILATED_DEPTH)] = FSR3FG_RES_DILATED_DEPTH_0 + h->shared_toggle;
    srv[FI(DILATED_MOTION_VECTORS)] = FSR3FG_RES_DILATED_MOTION_VECTORS_0 + h->shared_toggle;
    srv[FI(RECONSTRUCTED_DEPTH_PREVIOUS_FRAME)] = FSR3FG_RES_RECONSTRUCTED_PREVIOUS_DEPTH_0 + h->shared_toggle;
    uav[FI(OUTPUT)] = FSR3FG_RES_OUTPUT;
    srv[FI(OUTPUT)] = uav[FI(OUTPUT)];
    /* opticalFlowScale.x > 0: the provider's optical-flow results. */
    srv[FI(OPTICAL_FLOW_VECTOR)] = FSR3FG_RES_OF_VECTOR;
    srv[FI(OPTICAL_FLOW_SCENE_CHANGE_DETECTION)] = FSR3FG_RES_OF_SCD;
    srv[FI(DISTORTION_FIELD)] = srv[FI(DEFAULT_DISTORTION_FIELD)];

    const uint32_t display_x = (display_w + 7) / 8, display_y = (display_h + 7) / 8;
    const uint32_t render_x = (p->render_width + 7) / 8, render_y = (p->render_height + 7) / 8;
    /* uint32_t(displaySize / float(opticalFlowBlockSize) + 7) / 8 */
    const uint32_t flow_x = (uint32_t)((float)display_w / (float)OF_BLOCK_SIZE + 7.0f) / 8;
    const uint32_t flow_y = (uint32_t)((float)display_h / (float)OF_BLOCK_SIZE + 7.0f) / 8;

    static const uint32_t aliasable[] = {
        FI(RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME), FI(GAME_MOTION_VECTOR_FIELD_X), FI(GAME_MOTION_VECTOR_FIELD_Y),
        FI(INPAINTING_PYRAMID), FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_X), FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y),
        FI(INPAINTING_MASK), FI(DISOCCLUSION_MASK),
    };
    for (size_t i = 0; i < sizeof(aliasable) / sizeof(aliasable[0]); i++)
        job_barrier(h, uav[aliasable[i]], STATE_UNORDERED_ACCESS);
    for (size_t i = 0; i < sizeof(aliasable) / sizeof(aliasable[0]); i++) job_discard(h, uav[aliasable[i]]);

    job_dispatch(h, FSR3FG_PASS_SETUP, render_x, render_y, 1);

    /* The preparation passes run only when this dispatch does not reset. */
    if (!c->Reset) {
        job_clear(h, uav[FI(GAME_MOTION_VECTOR_FIELD_X)], 0);
        job_clear(h, uav[FI(GAME_MOTION_VECTOR_FIELD_Y)], 0);
        job_clear(h, uav[FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_X)], 0);
        job_clear(h, uav[FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y)], 0);
        /* "Clear Reconstructed Depth Interpolated Frame": 0 for inverted depth. */
        job_clear(h, uav[FI(RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME)], float_bits(0.0f));
        job_dispatch(h, FSR3FG_PASS_RECONSTRUCT_PREVIOUS_DEPTH, render_x, render_y, 1);
        job_dispatch(h, FSR3FG_PASS_GAME_MOTION_VECTOR_FIELD, render_x, render_y, 1);
        inpainting_pyramid(h, FSR3FG_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID, p->render_width, p->render_height);
        job_dispatch(h, FSR3FG_PASS_OPTICAL_FLOW_VECTOR_FIELD, flow_x, flow_y, 1);
        job_dispatch(h, FSR3FG_PASS_DISOCCLUSION_MASK, render_x, render_y, 1);
    }

    job_dispatch(h, FSR3FG_PASS_INTERPOLATION, display_x, display_y, 1);
    inpainting_pyramid(h, FSR3FG_PASS_INPAINTING_PYRAMID, display_w, display_h);
    job_dispatch(h, FSR3FG_PASS_INPAINTING, display_x, display_y, 1);

    /* Store the current interpolation source for the next dispatch. */
    job_copy(h, srv[FI(CURRENT_INTERPOLATION_SOURCE)], uav[FI(PREVIOUS_INTERPOLATION_SOURCE)]);

    static const uint32_t internal[] = {
        FI(RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME), FI(GAME_MOTION_VECTOR_FIELD_X), FI(GAME_MOTION_VECTOR_FIELD_Y),
        FI(INPAINTING_PYRAMID), FI(COUNTERS), FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_X),
        FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y), FI(PREVIOUS_INTERPOLATION_SOURCE), FI(DISOCCLUSION_MASK),
    };
    for (size_t i = 0; i < sizeof(internal) / sizeof(internal[0]); i++)
        job_barrier(h, srv[internal[i]], internal[i] == FI(COUNTERS) ? STATE_COPY_DEST : STATE_UNORDERED_ACCESS);
}

/* --- the provider's dispatch ------------------------------------------------------------------ */

int fsr3_fg_host_dispatch(Fsr3FgHost *h, const Fsr3FgHostDispatch *d) {
    const uint32_t *r = d->generation_rect;
    FiDispatch fi;
    if (r[2] == 0 && r[3] == 0) {
        fi.rect[0] = 0;
        fi.rect[1] = 0;
        fi.rect[2] = h->desc.display_width;
        fi.rect[3] = h->desc.display_height;
    } else {
        if (!r[2] || !r[3] || r[0] > h->desc.display_width || r[2] > h->desc.display_width - r[0] ||
            r[1] > h->desc.display_height || r[3] > h->desc.display_height - r[1])
            return -1;
        memcpy(fi.rect, r, sizeof(fi.rect));
    }
    plan_begin(h);

    const Fsr3FgHostPrepare *p = &h->prepared[d->frame_id % PROVIDER_MAX_QUEUED_FRAMES];
    /* Disjoint frame IDs: decreased, skipped, or not the frame last configured. */
    const int decreased = d->frame_id < h->last_frame_id;
    const int skipped = (d->frame_id - h->last_frame_id) > 1;
    const int disjoint = decreased || skipped || d->frame_id != h->last_configure_frame_id;
    const int reset = d->reset || p->reset || disjoint;

    of_dispatch(h, h->has_hudless ? FSR3FG_RES_HUDLESS_COLOR : FSR3FG_RES_PRESENT_COLOR, reset,
                d->backbuffer_transfer_function, d->min_max_luminance);

    fi.frame_id = d->frame_id;
    fi.reset = reset;
    fi.prepared = p;
    fi.transfer = d->backbuffer_transfer_function;
    fi.min_max[0] = d->min_max_luminance[0];
    fi.min_max[1] = d->min_max_luminance[1];
    fi_dispatch(h, &fi);

    h->last_frame_id = d->frame_id;
    return h->overflow ? -1 : 0;
}

/* --- pass layouts ------------------------------------------------------------------------------ */

/* The bindings of the project's SPIR-V (src/rt/gpu_sdl3vk/fsr3fg_*_comp.inc), each with what AMD's
 * patchResourceBindings tables resolve its name to. tools/test_fsr3_port.py checks them against the
 * modules. */
#define CB_FI FSR3FG_STAGE_FI
#define CB_INPAINTING FSR3FG_STAGE_INPAINTING
#define CB_OF FSR3FG_STAGE_OF
#define LINEAR_CLAMP 1u

static const Fsr3FgBinding reconstruct_and_dilate[] = {
    { 0, FSR3FG_BIND_CBV, CB_FI, 0, "cbFI" },
    { 8, FSR3FG_BIND_SRV, FI(MOTION_VECTORS), 0, "r_input_motion_vectors" },
    { 9, FSR3FG_BIND_SRV, FI(DEPTH), 0, "r_input_depth" },
    { 40, FSR3FG_BIND_UAV, FI(RECONSTRUCTED_DEPTH_PREVIOUS_FRAME), 0, "rw_reconstructed_depth_previous_frame" },
    { 41, FSR3FG_BIND_UAV, FI(DILATED_MOTION_VECTORS), 0, "rw_dilated_motion_vectors" },
    { 42, FSR3FG_BIND_UAV, FI(DILATED_DEPTH), 0, "rw_dilated_depth" },
};
static const Fsr3FgBinding setup[] = {
    { 0, FSR3FG_BIND_CBV, CB_FI, 0, "cbFI" },
    { 8, FSR3FG_BIND_SRV, FI(OPTICAL_FLOW_SCENE_CHANGE_DETECTION), 0, "r_optical_flow_scd" },
    { 44, FSR3FG_BIND_UAV, FI(DISOCCLUSION_MASK), 0, "rw_disocclusion_mask" },
    { 45, FSR3FG_BIND_UAV_BUFFER, FI(COUNTERS), 0, "rw_counters" },
};
static const Fsr3FgBinding reconstruct_previous_depth[] = {
    { 0, FSR3FG_BIND_CBV, CB_FI, 0, "cbFI" },
    { 4, FSR3FG_BIND_SAMPLER, LINEAR_CLAMP, 0, "s_LinearClamp" },
    { 8, FSR3FG_BIND_SRV, FI(DILATED_MOTION_VECTORS), 0, "r_dilated_motion_vectors" },
    { 9, FSR3FG_BIND_SRV, FI(DILATED_DEPTH), 0, "r_dilated_depth" },
    { 11, FSR3FG_BIND_SRV, FI(DISTORTION_FIELD), 1, "r_input_distortion_field" },
    { 40, FSR3FG_BIND_UAV, FI(RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME), 0, "rw_reconstructed_depth_interpolated_frame" },
};
static const Fsr3FgBinding game_motion_vector_field[] = {
    { 0, FSR3FG_BIND_CBV, CB_FI, 0, "cbFI" },
    { 4, FSR3FG_BIND_SAMPLER, LINEAR_CLAMP, 0, "s_LinearClamp" },
    { 8, FSR3FG_BIND_SRV, FI(DILATED_MOTION_VECTORS), 0, "r_dilated_motion_vectors" },
    { 9, FSR3FG_BIND_SRV, FI(DILATED_DEPTH), 0, "r_dilated_depth" },
    { 10, FSR3FG_BIND_SRV, FI(PREVIOUS_INTERPOLATION_SOURCE), 1, "r_previous_interpolation_source" },
    { 11, FSR3FG_BIND_SRV, FI(CURRENT_INTERPOLATION_SOURCE), 1, "r_current_interpolation_source" },
    { 12, FSR3FG_BIND_SRV, FI(DISTORTION_FIELD), 1, "r_input_distortion_field" },
    { 40, FSR3FG_BIND_UAV, FI(GAME_MOTION_VECTOR_FIELD_X), 0, "rw_game_motion_vector_field_x" },
    { 41, FSR3FG_BIND_UAV, FI(GAME_MOTION_VECTOR_FIELD_Y), 0, "rw_game_motion_vector_field_y" },
};
static const Fsr3FgBinding optical_flow_vector_field[] = {
    { 0, FSR3FG_BIND_CBV, CB_FI, 0, "cbFI" },
    { 4, FSR3FG_BIND_SAMPLER, LINEAR_CLAMP, 0, "s_LinearClamp" },
    { 8, FSR3FG_BIND_SRV, FI(OPTICAL_FLOW_VECTOR), 0, "r_optical_flow" },
    { 11, FSR3FG_BIND_SRV, FI(PREVIOUS_INTERPOLATION_SOURCE), 1, "r_previous_interpolation_source" },
    { 12, FSR3FG_BIND_SRV, FI(CURRENT_INTERPOLATION_SOURCE), 1, "r_current_interpolation_source" },
    { 40, FSR3FG_BIND_UAV, FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_X), 0, "rw_optical_flow_motion_vector_field_x" },
    { 41, FSR3FG_BIND_UAV, FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y), 0, "rw_optical_flow_motion_vector_field_y" },
};
static const Fsr3FgBinding disocclusion_mask[] = {
    { 0, FSR3FG_BIND_CBV, CB_FI, 0, "cbFI" },
    { 4, FSR3FG_BIND_SAMPLER, LINEAR_CLAMP, 0, "s_LinearClamp" },
    { 8, FSR3FG_BIND_SRV, FI(GAME_MOTION_VECTOR_FIELD_X), 0, "r_game_motion_vector_field_x" },
    { 9, FSR3FG_BIND_SRV, FI(GAME_MOTION_VECTOR_FIELD_Y), 0, "r_game_motion_vector_field_y" },
    { 10, FSR3FG_BIND_SRV, FI(RECONSTRUCTED_DEPTH_PREVIOUS_FRAME), 0, "r_reconstructed_depth_previous_frame" },
    { 11, FSR3FG_BIND_SRV, FI(DILATED_DEPTH), 0, "r_dilated_depth" },
    { 12, FSR3FG_BIND_SRV, FI(RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME), 0, "r_reconstructed_depth_interpolated_frame" },
    { 13, FSR3FG_BIND_SRV, FI(INPAINTING_PYRAMID), 0, "r_inpainting_pyramid" },
    { 14, FSR3FG_BIND_SRV, FI(DISTORTION_FIELD), 1, "r_input_distortion_field" },
    { 40, FSR3FG_BIND_UAV, FI(DISOCCLUSION_MASK), 0, "rw_disocclusion_mask" },
};
static const Fsr3FgBinding interpolation[] = {
    { 0, FSR3FG_BIND_CBV, CB_FI, 0, "cbFI" },
    { 4, FSR3FG_BIND_SAMPLER, LINEAR_CLAMP, 0, "s_LinearClamp" },
    { 8, FSR3FG_BIND_SRV, FI(GAME_MOTION_VECTOR_FIELD_X), 0, "r_game_motion_vector_field_x" },
    { 9, FSR3FG_BIND_SRV, FI(GAME_MOTION_VECTOR_FIELD_Y), 0, "r_game_motion_vector_field_y" },
    { 10, FSR3FG_BIND_SRV, FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_X), 0, "r_optical_flow_motion_vector_field_x" },
    { 11, FSR3FG_BIND_SRV, FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y), 0, "r_optical_flow_motion_vector_field_y" },
    { 12, FSR3FG_BIND_SRV, FI(PREVIOUS_INTERPOLATION_SOURCE), 0, "r_previous_interpolation_source" },
    { 13, FSR3FG_BIND_SRV, FI(CURRENT_INTERPOLATION_SOURCE), 0, "r_current_interpolation_source" },
    { 14, FSR3FG_BIND_SRV, FI(DISOCCLUSION_MASK), 1, "r_disocclusion_mask" },
    { 15, FSR3FG_BIND_SRV, FI(INPAINTING_PYRAMID), 0, "r_inpainting_pyramid" },
    { 16, FSR3FG_BIND_SRV_BUFFER, FI(COUNTERS), 0, "r_counters" },
    { 40, FSR3FG_BIND_UAV, FI(OUTPUT), 0, "rw_output" },
    { 41, FSR3FG_BIND_UAV, FI(INPAINTING_MASK), 0, "rw_inpainting_mask" },
};
static const Fsr3FgBinding inpainting_pyramid_pass[] = {
    { 0, FSR3FG_BIND_CBV, CB_FI, 0, "cbFI" },
    { 1, FSR3FG_BIND_CBV, CB_INPAINTING, 0, "cbInpaintingPyramid" },
    { 8, FSR3FG_BIND_SRV, FI(OUTPUT), 0, "r_output" },
    { 9, FSR3FG_BIND_SRV, FI(INPAINTING_MASK), 0, "r_inpainting_mask" },
    { 40, FSR3FG_BIND_UAV_BUFFER, FI(COUNTERS), 0, "rw_counters" },
    { 41, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_0), 0, "rw_inpainting_pyramid0" },
    { 42, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_1), 0, "rw_inpainting_pyramid1" },
    { 43, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_2), 0, "rw_inpainting_pyramid2" },
    { 44, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_3), 0, "rw_inpainting_pyramid3" },
    { 45, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_4), 0, "rw_inpainting_pyramid4" },
    { 46, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_5), 0, "rw_inpainting_pyramid5" },
    { 47, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_6), 0, "rw_inpainting_pyramid6" },
    { 48, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_7), 0, "rw_inpainting_pyramid7" },
    { 49, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_8), 0, "rw_inpainting_pyramid8" },
    { 50, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_9), 0, "rw_inpainting_pyramid9" },
    { 51, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_10), 0, "rw_inpainting_pyramid10" },
    { 52, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_11), 0, "rw_inpainting_pyramid11" },
};
static const Fsr3FgBinding inpainting[] = {
    { 0, FSR3FG_BIND_CBV, CB_FI, 0, "cbFI" },
    { 8, FSR3FG_BIND_SRV, FI(OPTICAL_FLOW_SCENE_CHANGE_DETECTION), 0, "r_optical_flow_scd" },
    { 9, FSR3FG_BIND_SRV, FI(INPAINTING_PYRAMID), 0, "r_inpainting_pyramid" },
    { 10, FSR3FG_BIND_SRV, FI(PRESENT_BACKBUFFER), 0, "r_present_backbuffer" },
    { 11, FSR3FG_BIND_SRV, FI(CURRENT_INTERPOLATION_SOURCE), 0, "r_current_interpolation_source" },
    { 40, FSR3FG_BIND_UAV, FI(OUTPUT), 0, "rw_output" },
    { 41, FSR3FG_BIND_UAV, FI(INPAINTING_MASK), 0, "rw_inpainting_mask" },
};
static const Fsr3FgBinding game_vector_field_inpainting_pyramid[] = {
    { 0, FSR3FG_BIND_CBV, CB_FI, 0, "cbFI" },
    { 1, FSR3FG_BIND_CBV, CB_INPAINTING, 0, "cbInpaintingPyramid" },
    { 8, FSR3FG_BIND_SRV, FI(GAME_MOTION_VECTOR_FIELD_X), 0, "r_game_motion_vector_field_x" },
    { 9, FSR3FG_BIND_SRV, FI(GAME_MOTION_VECTOR_FIELD_Y), 0, "r_game_motion_vector_field_y" },
    { 40, FSR3FG_BIND_UAV_BUFFER, FI(COUNTERS), 0, "rw_counters" },
    { 41, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_0), 0, "rw_inpainting_pyramid0" },
    { 42, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_1), 0, "rw_inpainting_pyramid1" },
    { 43, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_2), 0, "rw_inpainting_pyramid2" },
    { 44, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_3), 0, "rw_inpainting_pyramid3" },
    { 45, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_4), 0, "rw_inpainting_pyramid4" },
    { 46, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_5), 0, "rw_inpainting_pyramid5" },
    { 47, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_6), 0, "rw_inpainting_pyramid6" },
    { 48, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_7), 0, "rw_inpainting_pyramid7" },
    { 49, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_8), 0, "rw_inpainting_pyramid8" },
    { 50, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_9), 0, "rw_inpainting_pyramid9" },
    { 51, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_10), 0, "rw_inpainting_pyramid10" },
    { 52, FSR3FG_BIND_UAV, FI(INPAINTING_PYRAMID_MIPMAP_11), 0, "rw_inpainting_pyramid11" },
};
static const Fsr3FgBinding of_prepare_luma[] = {
    { 0, FSR3FG_BIND_CBV, CB_OF, 0, "cbOF" },
    { 8, FSR3FG_BIND_SRV, OFB_INPUT_COLOR, 0, "r_input_color" },
    { 40, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_INPUT, 0, "rw_optical_flow_input" },
};
static const Fsr3FgBinding of_input_pyramid[] = {
    { 40, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_INPUT, 0, "rw_optical_flow_input" },
    { 41, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_INPUT_LEVEL_1, 0, "rw_optical_flow_input_level_1" },
    { 42, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_INPUT_LEVEL_2, 0, "rw_optical_flow_input_level_2" },
    { 43, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_INPUT_LEVEL_3, 0, "rw_optical_flow_input_level_3" },
    { 44, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_INPUT_LEVEL_4, 0, "rw_optical_flow_input_level_4" },
    { 45, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_INPUT_LEVEL_5, 0, "rw_optical_flow_input_level_5" },
    { 46, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_INPUT_LEVEL_6, 0, "rw_optical_flow_input_level_6" },
};
static const Fsr3FgBinding of_scd_histogram[] = {
    { 0, FSR3FG_BIND_CBV, CB_OF, 0, "cbOF" },
    { 8, FSR3FG_BIND_SRV, OFB_OPTICAL_FLOW_INPUT, 0, "r_optical_flow_input" },
    { 40, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_SCD_HISTOGRAM, 0, "rw_optical_flow_scd_histogram" },
};
static const Fsr3FgBinding of_scd_divergence[] = {
    { 40, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_SCD_HISTOGRAM, 0, "rw_optical_flow_scd_histogram" },
    { 41, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM, 0, "rw_optical_flow_scd_previous_histogram" },
    { 42, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_SCD_TEMP, 0, "rw_optical_flow_scd_temp" },
    { 43, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_SCD_OUTPUT, 0, "rw_optical_flow_scd_output" },
};
static const Fsr3FgBinding of_search[] = {
    { 0, FSR3FG_BIND_CBV, CB_OF, 0, "cbOF" },
    { 8, FSR3FG_BIND_SRV, OFB_OPTICAL_FLOW_INPUT, 0, "r_optical_flow_input" },
    { 9, FSR3FG_BIND_SRV, OFB_OPTICAL_FLOW_PREVIOUS_INPUT, 0, "r_optical_flow_previous_input" },
    { 40, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW, 0, "rw_optical_flow" },
    { 41, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_SCD_OUTPUT, 0, "rw_optical_flow_scd_output" },
};
static const Fsr3FgBinding of_filter[] = {
    { 8, FSR3FG_BIND_SRV, OFB_OPTICAL_FLOW_PREVIOUS, 0, "r_optical_flow_previous" },
    { 40, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW, 0, "rw_optical_flow" },
};
static const Fsr3FgBinding of_scale[] = {
    { 0, FSR3FG_BIND_CBV, CB_OF, 0, "cbOF" },
    { 8, FSR3FG_BIND_SRV, OFB_OPTICAL_FLOW_INPUT, 0, "r_optical_flow_input" },
    { 9, FSR3FG_BIND_SRV, OFB_OPTICAL_FLOW_PREVIOUS_INPUT, 0, "r_optical_flow_previous_input" },
    { 10, FSR3FG_BIND_SRV, OFB_OPTICAL_FLOW, 0, "r_optical_flow" },
    { 40, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_NEXT_LEVEL, 0, "rw_optical_flow_next_level" },
    { 41, FSR3FG_BIND_UAV, OFB_OPTICAL_FLOW_SCD_OUTPUT, 0, "rw_optical_flow_scd_output" },
};

#define LAYOUT(label, x, y, z, of, s32, table) \
    { label, { x, y, z }, of, s32, (uint32_t)(sizeof(table) / sizeof((table)[0])), table }
const Fsr3FgPassLayout fsr3_fg_pass_layout[FSR3FG_PASS_COUNT] = {
    LAYOUT("RECONSTRUCT_AND_DILATE", 8, 8, 1, 0, 0, reconstruct_and_dilate),
    LAYOUT("SETUP", 8, 8, 1, 0, 0, setup),
    LAYOUT("RECONSTRUCT_PREV_DEPTH", 8, 8, 1, 0, 0, reconstruct_previous_depth),
    LAYOUT("GAME_MOTION_VECTOR_FIELD", 8, 8, 1, 0, 0, game_motion_vector_field),
    LAYOUT("OPTICAL_FLOW_VECTOR_FIELD", 8, 8, 1, 0, 0, optical_flow_vector_field),
    LAYOUT("DISOCCLUSION_MASK", 8, 8, 1, 0, 0, disocclusion_mask),
    LAYOUT("INTERPOLATION", 8, 8, 1, 0, 0, interpolation),
    LAYOUT("INPAINTING_PYRAMID", 256, 1, 1, 0, 0, inpainting_pyramid_pass),
    LAYOUT("INPAINTING", 8, 8, 1, 0, 0, inpainting),
    LAYOUT("GAME_VECTOR_FIELD_INPAINTING_PYRAMID", 256, 1, 1, 0, 0, game_vector_field_inpainting_pyramid),
    LAYOUT("Opticalflow_Luma", 16, 16, 1, 1, 0, of_prepare_luma),
    LAYOUT("Opticalflow_InputPyramid", 256, 1, 1, 1, 0, of_input_pyramid),
    LAYOUT("Opticalflow_SCD_Histogram", 32, 8, 1, 1, 0, of_scd_histogram),
    LAYOUT("Opticalflow_SCD_Divergence", 256, 1, 1, 1, 0, of_scd_divergence),
    LAYOUT("Opticalflow_Search", 64, 1, 1, 1, 1, of_search),
    LAYOUT("Opticalflow_Filter", 16, 4, 1, 1, 0, of_filter),
    LAYOUT("Opticalflow_Upscale", 4, 4, 4, 1, 0, of_scale),
};
