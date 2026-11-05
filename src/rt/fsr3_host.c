/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * Derived from the AMD FidelityFX SDK v2.4.0 (commit 7539ae8b6568ad29ed78a753404351daf3b5cca7), under the MIT
 * licence below, from these files under Kits/FidelityFX/:
 *   upscalers/fsr3/internal/ffx_fsr3upscaler.cpp (the host: creation, the dispatch plan, constants, clears
 *     and discards);
 *   upscalers/fsr3/internal/ffx_fsr3upscaler_private.h (the constant-buffer layout);
 *   upscalers/fsr3/include/ffx_fsr3upscaler.h (the creation flags);
 *   upscalers/fsr3/include/gpu/spd/ffx_spd.h (ffxSpdSetup);
 *   upscalers/fsr3/include/gpu/fsr1/ffx_fsr1.h (FsrRcasCon);
 *   api/internal/gpu/ffx_core_cpu.h (ffxF32ToF16).
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
/* fsr3_host.c - see fsr3_host.h. Comments name the AMD function each part translates. */
#include "fsr3_host.h"
#include "fsr3_jitter.h"

#include <float.h>
#include <math.h>
#include <string.h>

#define FFX_CPU 1
#include "fsr3upscaler/ffx_fsr3upscaler_resources.h"

typedef char fsr3_constants_size_is_amds[sizeof(Fsr3Constants) == 148 ? 1 : -1];

#define FSR3_MAX_QUEUED_FRAMES 16u /* FSR3UPSCALER_MAX_QUEUED_FRAMES; must be even */

/* --- ffxSpdSetup, FsrRcasCon, ffxF32ToF16 --------------------------------------------------- */

void fsr3_spd_setup(uint32_t dispatch_groups[2], uint32_t work_group_offset[2],
                    uint32_t num_work_groups_and_mips[2], const uint32_t rect[4]) {
    work_group_offset[0] = rect[0] / 64;
    work_group_offset[1] = rect[1] / 64;
    const uint32_t end_x = (rect[0] + rect[2] - 1) / 64;
    const uint32_t end_y = (rect[1] + rect[3] - 1) / 64;
    dispatch_groups[0] = end_x + 1 - work_group_offset[0];
    dispatch_groups[1] = end_y + 1 - work_group_offset[1];
    num_work_groups_and_mips[0] = dispatch_groups[0] * dispatch_groups[1];
    /* mips = -1: from the larger side, floor(log2(resolution)) capped at 12. */
    const uint32_t resolution = rect[2] > rect[3] ? rect[2] : rect[3];
    const float m = floorf(log2f((float)resolution));
    num_work_groups_and_mips[1] = (uint32_t)(m < 12.0f ? m : 12.0f);
}

uint32_t fsr3_f32_to_f16(float f) {
    /* ffxF32ToF16's 512-entry tables, generated: truncation, denormals kept, and every exponent
     * above the half range (infinity and NaN too) saturating to the largest finite half. */
    union { float f; uint32_t u; } bits;
    bits.f = f;
    const uint32_t i = bits.u >> 23;
    const int e = (int)(i & 255u) - 127;
    const uint32_t sign = (i & 256u) ? 0x8000u : 0u;
    uint32_t base, shift;
    if (e < -24) { base = 0; shift = 24; }
    else if (e < -14) { base = 0x0400u >> (-e - 14); shift = (uint32_t)(-e - 1); }
    else if (e <= 15) { base = (uint32_t)(e + 15) << 10; shift = 13; }
    else { base = 0x7bffu; shift = 24; }
    return (base | sign) + ((bits.u & 0x7fffffu) >> shift);
}

void fsr3_rcas_con(uint32_t con[4], float sharpness) {
    union { float f; uint32_t u; } bits;
    sharpness = exp2f(-sharpness);
    bits.f = sharpness;
    con[0] = bits.u;
    con[1] = fsr3_f32_to_f16(sharpness) + (fsr3_f32_to_f16(sharpness) << 16);
    con[2] = 0;
    con[3] = 0;
}

/* --- fsr3upscalerCreate --------------------------------------------------------------------- */

void fsr3_host_init(Fsr3Host *h, const Fsr3HostDesc *desc) {
    memset(h, 0, sizeof(*h));
    h->desc = *desc;
    h->first_execution = 1;
    h->resource_frame_index = 0;
    h->constants.maxUpscaleSize[0] = (int32_t)desc->max_upscale_width;
    h->constants.maxUpscaleSize[1] = (int32_t)desc->max_upscale_height;
    h->constants.velocityFactor = 1.0f;
    h->constants.reactivenessScale = 1.0f;
    h->constants.shadingChangeScale = 1.0f;
    h->constants.accumulationAddedPerFrame = 1.0f / 3.0f;
    h->constants.minDisocclusionAccumulation = -1.0f / 3.0f;
    /* srvResources and uavResources start as the created resources under their own names. */
    for (uint32_t r = 0; r < FSR3_HOST_RESOURCE_COUNT; r++) h->srv[r] = h->uav[r] = r;
}

/* --- fsr3upscalerDispatch ------------------------------------------------------------------- */

static void job_clear(Fsr3Host *h, uint32_t resource, float a, float b, float c, float d) {
    Fsr3Job *j = &h->jobs[h->job_count++];
    memset(j, 0, sizeof(*j));
    j->kind = FSR3_JOB_CLEAR;
    j->resource = resource;
    j->value[0] = a;
    j->value[1] = b;
    j->value[2] = c;
    j->value[3] = d;
}

static void job_discard(Fsr3Host *h, uint32_t resource) {
    Fsr3Job *j = &h->jobs[h->job_count++];
    memset(j, 0, sizeof(*j));
    j->kind = FSR3_JOB_DISCARD;
    j->resource = resource;
}

static void job_dispatch(Fsr3Host *h, Fsr3PassId pass, uint32_t x, uint32_t y) {
    Fsr3Job *j = &h->jobs[h->job_count++];
    memset(j, 0, sizeof(*j));
    j->kind = FSR3_JOB_DISPATCH;
    j->pass = pass;
    j->groups_x = x;
    j->groups_y = y;
}

/* setupDeviceDepthToViewSpaceDepthParams */
static void device_depth_to_view(Fsr3Host *h, const Fsr3HostDispatch *d) {
    const int inverted = (h->desc.flags & FSR3_HOST_ENABLE_DEPTH_INVERTED) != 0;
    const int infinite = (h->desc.flags & FSR3_HOST_ENABLE_DEPTH_INFINITE) != 0;
    float fmin = d->camera_near < d->camera_far ? d->camera_near : d->camera_far;
    float fmax = d->camera_near < d->camera_far ? d->camera_far : d->camera_near;
    if (inverted) {
        const float t = fmin;
        fmin = fmax;
        fmax = t;
    }
    const float q = fmax / (fmin - fmax);
    const float dd = -1.0f;
    const float elem_c[2][2] = { { q, -1.0f - FLT_EPSILON }, { q, 0.0f + FLT_EPSILON } };
    const float elem_e[2][2] = { { q * fmin, -fmin - FLT_EPSILON }, { q * fmin, fmax } };
    h->constants.deviceToViewDepth[0] = dd * elem_c[inverted][infinite];
    h->constants.deviceToViewDepth[1] = elem_e[inverted][infinite];
    const float aspect = (float)d->render_width / (float)d->render_height;
    const float cot_half_fov_y =
        cosf(0.5f * d->camera_fov_angle_vertical) / sinf(0.5f * d->camera_fov_angle_vertical);
    const float a = cot_half_fov_y / aspect;
    const float b = cot_half_fov_y;
    h->constants.deviceToViewDepth[2] = 1.0f / a;
    h->constants.deviceToViewDepth[3] = 1.0f / b;
}

static float clamp01(float v) {
    /* FFX_MAXIMUM(0.0f, FFX_MINIMUM(1.0f, v)) */
    const float lo = 1.0f < v ? 1.0f : v;
    return 0.0f > lo ? 0.0f : lo;
}

int fsr3_host_dispatch(Fsr3Host *h, const Fsr3HostDispatch *d) {
    if (!d->render_width || !d->render_height || d->render_width > h->desc.max_render_width ||
        d->render_height > h->desc.max_render_height)
        return -1;
    if ((d->upscale_width || d->upscale_height) &&
        (!d->upscale_width || !d->upscale_height || d->upscale_width > h->desc.max_upscale_width ||
         d->upscale_height > h->desc.max_upscale_height))
        return -1;
    h->job_count = 0;
    Fsr3Constants *c = &h->constants;
    uint32_t *srv = h->srv, *uav = h->uav;

    if (h->first_execution) {
        job_clear(h, srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1], 0, 0, 0, 0);
        job_clear(h, srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2], 0, 0, 0, 0);
        job_clear(h, srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1], 0, 0, 0, 0);
        job_clear(h, srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2], 0, 0, 0, 0);
    }

    const int odd = (h->resource_frame_index & 1u) != 0;
    const uint32_t acc_srv = odd ? FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2
                                 : FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1;
    const uint32_t acc_uav = odd ? FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_1
                                 : FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION_2;
    const uint32_t up_srv = odd ? FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2
                                : FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1;
    const uint32_t up_uav = odd ? FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_1
                                : FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR_2;
    const uint32_t hist_srv = odd ? FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2
                                  : FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1;
    const uint32_t hist_uav = odd ? FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_1
                                  : FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY_2;
    const uint32_t luma_cur = odd ? FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2
                                  : FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1;
    const uint32_t luma_prev = odd ? FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_1
                                   : FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_2;

    const int reset = d->reset || h->first_execution;
    h->first_execution = 0;

    /* Registered resources: the caller's images, under their own names. */
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR] = FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_COLOR;
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_DEPTH] = FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_DEPTH;
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS] =
        FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_MOTION_VECTORS;
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS] =
        FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS;
    uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS] =
        srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_MOTION_VECTORS];
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH] = FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH;
    uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH] = srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_DEPTH];
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH] =
        FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH;
    uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH] =
        srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH];

    if (h->desc.flags & FSR3_HOST_ENABLE_AUTO_EXPOSURE)
        srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE] = srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO];
    else if (!d->has_exposure)
        srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE] =
            srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_EXPOSURE];
    else
        srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE] = FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_EXPOSURE;
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_REACTIVE_MASK] = d->has_reactive
        ? FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_REACTIVE_MASK
        : srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY];
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_TRANSPARENCY_AND_COMPOSITION_MASK] =
        d->has_transparency_and_composition
            ? FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INPUT_TRANSPARENCY_AND_COMPOSITION_MASK
            : srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_DEFAULT_REACTIVITY];

    uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT] = FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_UPSCALED_OUTPUT;
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION] = srv[acc_srv];
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR] = srv[up_srv];
    uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_ACCUMULATION] = uav[acc_uav];
    uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERNAL_UPSCALED_COLOR] = uav[up_uav];
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_RCAS_INPUT] = uav[up_uav];
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA] = srv[luma_cur];
    uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_CURRENT_LUMA] = uav[luma_cur];
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_PREVIOUS_LUMA] = srv[luma_prev];
    uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY] = uav[hist_uav];
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_HISTORY] = srv[hist_srv];
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH] = srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1];
    uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH] = uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1];
    srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY] =
        srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1];
    uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_LUMA_INSTABILITY] =
        uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1];

    c->previousFrameJitterOffset[0] = c->jitterOffset[0];
    c->previousFrameJitterOffset[1] = c->jitterOffset[1];
    c->jitterOffset[0] = d->jitter_x;
    c->jitterOffset[1] = d->jitter_y;

    c->previousFrameRenderSize[0] = c->renderSize[0];
    c->previousFrameRenderSize[1] = c->renderSize[1];
    /* AMD falls back to the colour resource's size when renderSize is 0; this host refuses 0. */
    c->renderSize[0] = (int32_t)d->render_width;
    c->renderSize[1] = (int32_t)d->render_height;
    c->maxRenderSize[0] = (int32_t)h->desc.max_render_width;
    c->maxRenderSize[1] = (int32_t)h->desc.max_render_height;

    /* The horizontal FOV from the vertical one. AMD writes atan(tan(fov / 2) * aspectRatio) * 2 on
     * floats; its reference build (MSVC) takes <cmath>'s float overloads, atanf and tanf. */
    const float aspect_ratio = (float)d->render_width / (float)d->render_height;
    const float camera_angle_horizontal = atanf(tanf(d->camera_fov_angle_vertical / 2.0f) * aspect_ratio) * 2.0f;
    c->tanHalfFOV = tanf(camera_angle_horizontal * 0.5f);
    c->viewSpaceToMetersFactor = d->view_space_to_meters_factor > 0.0f ? d->view_space_to_meters_factor : 1.0f;

    device_depth_to_view(h, d);

    c->previousFrameUpscaleSize[0] = c->upscaleSize[0];
    c->previousFrameUpscaleSize[1] = c->upscaleSize[1];
    if (d->upscale_height == 0 && d->upscale_width == 0) {
        c->upscaleSize[0] = (int32_t)h->desc.max_upscale_width;
        c->upscaleSize[1] = (int32_t)h->desc.max_upscale_height;
    } else {
        c->upscaleSize[0] = (int32_t)d->upscale_width;
        c->upscaleSize[1] = (int32_t)d->upscale_height;
    }
    c->downscaleFactor[0] = (float)c->renderSize[0] / (float)c->upscaleSize[0];
    c->downscaleFactor[1] = (float)c->renderSize[1] / (float)c->upscaleSize[1];

    c->deltaPreExposure = 1.0f;
    h->previous_frame_pre_exposure = h->pre_exposure;
    h->pre_exposure = d->pre_exposure != 0.0f ? d->pre_exposure : 1.0f;
    if (h->previous_frame_pre_exposure > 0.0f)
        c->deltaPreExposure = h->pre_exposure / h->previous_frame_pre_exposure;

    const int32_t *mv_target = (h->desc.flags & FSR3_HOST_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS)
        ? c->upscaleSize : c->renderSize;
    c->motionVectorScale[0] = d->motion_vector_scale_x / (float)mv_target[0];
    c->motionVectorScale[1] = d->motion_vector_scale_y / (float)mv_target[1];
    if (h->desc.flags & FSR3_HOST_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION) {
        c->motionVectorJitterCancellation[0] =
            (h->previous_jitter_offset[0] - c->jitterOffset[0]) / (float)mv_target[0];
        c->motionVectorJitterCancellation[1] =
            (h->previous_jitter_offset[1] - c->jitterOffset[1]) / (float)mv_target[1];
        h->previous_jitter_offset[0] = c->jitterOffset[0];
        h->previous_jitter_offset[1] = c->jitterOffset[1];
    }

    const int32_t phase_count = fsr3_jitter_phase_count((int32_t)d->render_width, c->upscaleSize[0]);
    c->jitterPhaseCount = fsr3_jitter_ramp(c->jitterPhaseCount, phase_count, reset);

    /* Delta time to seconds, clamped to [0, 1]. */
    c->deltaTime = clamp01(d->frame_time_delta_ms / 1000.0f);
    c->frameIndex = reset ? 0.0f : c->frameIndex + 1.0f;

    const int32_t region = 8;
    const uint32_t src_x = (uint32_t)((c->renderSize[0] + (region - 1)) / region);
    const uint32_t src_y = (uint32_t)((c->renderSize[1] + (region - 1)) / region);
    const uint32_t dst_x = (uint32_t)((c->upscaleSize[0] + (region - 1)) / region);
    const uint32_t dst_y = (uint32_t)((c->upscaleSize[1] + (region - 1)) / region);
    const uint32_t sc_x = (uint32_t)(((int32_t)((float)c->renderSize[0] * 0.5f) + (region - 1)) / region);
    const uint32_t sc_y = (uint32_t)(((int32_t)((float)c->renderSize[1] * 0.5f) + (region - 1)) / region);

    if (reset) {
        job_clear(h, srv[acc_srv], 0, 0, 0, 0);
        job_clear(h, srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS], 0, 0, 0, 0);
        job_clear(h, srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_FRAME_INFO], -1.0f, 1.0f, 0, 0);
    }
    {
        const float v = (h->desc.flags & FSR3_HOST_ENABLE_DEPTH_INVERTED) ? 0.0f : 1.0f;
        job_clear(h, srv[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH], v, v, v, v);
    }
    job_clear(h, uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_ATOMIC_COUNT], 0, 0, 0, 0);

    uint32_t spd_groups[2], spd_offset[2], spd_wg_mips[2];
    const uint32_t rect[4] = { 0, 0, d->render_width, d->render_height };
    fsr3_spd_setup(spd_groups, spd_offset, spd_wg_mips, rect);
    h->spd.numworkGroups = spd_wg_mips[0];
    h->spd.mips = spd_wg_mips[1];
    h->spd.workGroupOffset[0] = spd_offset[0];
    h->spd.workGroupOffset[1] = spd_offset[1];
    h->spd.renderSize[0] = d->render_width;
    h->spd.renderSize[1] = d->render_height;

    memset(&h->rcas, 0, sizeof(h->rcas));
    fsr3_rcas_con(h->rcas.rcasConfig, (-2.0f * d->sharpness) + 2.0f);

    /* The aliasable intermediates lose their contents every frame (AMD's discard jobs), and the
     * SPD mips are cleared every frame (AMD labels that clear "Spd Atomic Count", but its target is
     * the SPD mip chain). */
    job_discard(h, uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_INTERMEDIATE_FP16x1]);
    job_discard(h, uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_SHADING_CHANGE]);
    job_discard(h, uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_NEW_LOCKS]);
    job_discard(h, uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_FARTHEST_DEPTH_MIP1]);
    job_discard(h, uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_DILATED_REACTIVE_MASKS]);
    job_clear(h, uav[FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_SPD_MIPS], 0, 0, 0, 0);

    job_dispatch(h, FSR3_PASS_PREPARE_INPUTS, src_x, src_y);
    job_dispatch(h, FSR3_PASS_LUMA_PYRAMID, spd_groups[0], spd_groups[1]);
    job_dispatch(h, FSR3_PASS_SHADING_CHANGE_PYRAMID, spd_groups[0], spd_groups[1]);
    job_dispatch(h, FSR3_PASS_SHADING_CHANGE, sc_x, sc_y);
    job_dispatch(h, FSR3_PASS_PREPARE_REACTIVITY, src_x, src_y);
    job_dispatch(h, FSR3_PASS_LUMA_INSTABILITY, src_x, src_y);
    /* AMD selects the sharpening accumulate permutation when sharpening. This project builds only
     * APPLY_SHARPENING=0, which also writes the output; RCAS then overwrites every output pixel
     * from the same internal colour, so the result is the same. */
    job_dispatch(h, FSR3_PASS_ACCUMULATE, dst_x, dst_y);
    if (d->enable_sharpening) {
        const int32_t rcas_region = 16;
        job_dispatch(h, FSR3_PASS_RCAS, (uint32_t)((c->upscaleSize[0] + (rcas_region - 1)) / rcas_region),
                     (uint32_t)((c->upscaleSize[1] + (rcas_region - 1)) / rcas_region));
    }

    h->resource_frame_index = (h->resource_frame_index + 1) % FSR3_MAX_QUEUED_FRAMES;
    return 0;
}

/* --- pass layouts and scheduleDispatch's lookup --------------------------------------------- */

#define R(x) FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_##x
#define CB_MAIN FFX_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER
#define CB_SPD FFX_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD
#define CB_RCAS FFX_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS
#define LINEAR_CLAMP 1u /* createPipelineStates' samplers: 0 point clamp, 1 linear clamp */

static const Fsr3Binding prepare_inputs[] = {
    { 0, FSR3_BIND_CBV, CB_MAIN, 0, "cbFSR3Upscaler" },
    { 8, FSR3_BIND_SRV, R(INPUT_MOTION_VECTORS), 0, "r_input_motion_vectors" },
    { 9, FSR3_BIND_SRV, R(INPUT_DEPTH), 0, "r_input_depth" },
    { 10, FSR3_BIND_SRV, R(INPUT_COLOR), 0, "r_input_color_jittered" },
    { 40, FSR3_BIND_UAV, R(DILATED_MOTION_VECTORS), 0, "rw_dilated_motion_vectors" },
    { 41, FSR3_BIND_UAV, R(DILATED_DEPTH), 0, "rw_dilated_depth" },
    { 42, FSR3_BIND_UAV, R(RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH), 0, "rw_reconstructed_previous_nearest_depth" },
    { 43, FSR3_BIND_UAV, R(FARTHEST_DEPTH), 0, "rw_farthest_depth" },
    { 44, FSR3_BIND_UAV, R(CURRENT_LUMA), 0, "rw_current_luma" },
};
static const Fsr3Binding luma_pyramid[] = {
    { 0, FSR3_BIND_CBV, CB_MAIN, 0, "cbFSR3Upscaler" },
    { 1, FSR3_BIND_CBV, CB_SPD, 0, "cbSPD" },
    { 8, FSR3_BIND_SRV, R(CURRENT_LUMA), 0, "r_current_luma" },
    { 9, FSR3_BIND_SRV, R(FARTHEST_DEPTH), 0, "r_farthest_depth" },
    { 40, FSR3_BIND_UAV, R(SPD_ATOMIC_COUNT), 0, "rw_spd_global_atomic" },
    { 41, FSR3_BIND_UAV, R(FRAME_INFO), 0, "rw_frame_info" },
    { 47, FSR3_BIND_UAV, R(SPD_MIPS_LEVEL_5), 0, "rw_spd_mip5" },
    { 48, FSR3_BIND_UAV, R(FARTHEST_DEPTH_MIP1), 0, "rw_farthest_depth_mip1" },
};
static const Fsr3Binding shading_change_pyramid[] = {
    { 0, FSR3_BIND_CBV, CB_MAIN, 0, "cbFSR3Upscaler" },
    { 1, FSR3_BIND_CBV, CB_SPD, 0, "cbSPD" },
    { 8, FSR3_BIND_SRV, R(CURRENT_LUMA), 0, "r_current_luma" },
    { 9, FSR3_BIND_SRV, R(PREVIOUS_LUMA), 0, "r_previous_luma" },
    { 10, FSR3_BIND_SRV, R(DILATED_MOTION_VECTORS), 0, "r_dilated_motion_vectors" },
    { 11, FSR3_BIND_SRV, R(INPUT_EXPOSURE), 0, "r_input_exposure" },
    { 40, FSR3_BIND_UAV, R(SPD_ATOMIC_COUNT), 0, "rw_spd_global_atomic" },
    { 41, FSR3_BIND_UAV, R(SPD_MIPS_LEVEL_0), 0, "rw_spd_mip0" },
    { 42, FSR3_BIND_UAV, R(SPD_MIPS_LEVEL_1), 0, "rw_spd_mip1" },
    { 43, FSR3_BIND_UAV, R(SPD_MIPS_LEVEL_2), 0, "rw_spd_mip2" },
    { 44, FSR3_BIND_UAV, R(SPD_MIPS_LEVEL_3), 0, "rw_spd_mip3" },
    { 45, FSR3_BIND_UAV, R(SPD_MIPS_LEVEL_4), 0, "rw_spd_mip4" },
    { 46, FSR3_BIND_UAV, R(SPD_MIPS_LEVEL_5), 0, "rw_spd_mip5" },
};
static const Fsr3Binding shading_change[] = {
    { 0, FSR3_BIND_CBV, CB_MAIN, 0, "cbFSR3Upscaler" },
    { 5, FSR3_BIND_SAMPLER, LINEAR_CLAMP, 0, "s_LinearClamp" },
    { 8, FSR3_BIND_SRV, R(SPD_MIPS), 1, "r_spd_mips" },
    { 40, FSR3_BIND_UAV, R(SHADING_CHANGE), 0, "rw_shading_change" },
};
static const Fsr3Binding prepare_reactivity[] = {
    { 0, FSR3_BIND_CBV, CB_MAIN, 0, "cbFSR3Upscaler" },
    { 5, FSR3_BIND_SAMPLER, LINEAR_CLAMP, 0, "s_LinearClamp" },
    { 8, FSR3_BIND_SRV, R(RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH), 0, "r_reconstructed_previous_nearest_depth" },
    { 9, FSR3_BIND_SRV, R(DILATED_MOTION_VECTORS), 0, "r_dilated_motion_vectors" },
    { 10, FSR3_BIND_SRV, R(DILATED_DEPTH), 0, "r_dilated_depth" },
    { 11, FSR3_BIND_SRV, R(INPUT_REACTIVE_MASK), 0, "r_reactive_mask" },
    { 12, FSR3_BIND_SRV, R(INPUT_TRANSPARENCY_AND_COMPOSITION_MASK), 1, "r_transparency_and_composition_mask" },
    { 13, FSR3_BIND_SRV, R(ACCUMULATION), 1, "r_accumulation" },
    { 14, FSR3_BIND_SRV, R(SHADING_CHANGE), 1, "r_shading_change" },
    { 15, FSR3_BIND_SRV, R(CURRENT_LUMA), 0, "r_current_luma" },
    { 16, FSR3_BIND_SRV, R(INPUT_EXPOSURE), 0, "r_input_exposure" },
    { 40, FSR3_BIND_UAV, R(DILATED_REACTIVE_MASKS), 0, "rw_dilated_reactive_masks" },
    { 41, FSR3_BIND_UAV, R(NEW_LOCKS), 0, "rw_new_locks" },
    { 42, FSR3_BIND_UAV, R(ACCUMULATION), 0, "rw_accumulation" },
};
static const Fsr3Binding luma_instability[] = {
    { 0, FSR3_BIND_CBV, CB_MAIN, 0, "cbFSR3Upscaler" },
    { 5, FSR3_BIND_SAMPLER, LINEAR_CLAMP, 0, "s_LinearClamp" },
    { 8, FSR3_BIND_SRV, R(INPUT_EXPOSURE), 0, "r_input_exposure" },
    { 9, FSR3_BIND_SRV, R(DILATED_REACTIVE_MASKS), 1, "r_dilated_reactive_masks" },
    { 10, FSR3_BIND_SRV, R(DILATED_MOTION_VECTORS), 0, "r_dilated_motion_vectors" },
    { 12, FSR3_BIND_SRV, R(LUMA_HISTORY), 1, "r_luma_history" },
    { 14, FSR3_BIND_SRV, R(CURRENT_LUMA), 1, "r_current_luma" },
    { 40, FSR3_BIND_UAV, R(LUMA_HISTORY), 0, "rw_luma_history" },
    { 41, FSR3_BIND_UAV, R(LUMA_INSTABILITY), 0, "rw_luma_instability" },
};
static const Fsr3Binding accumulate[] = {
    { 0, FSR3_BIND_CBV, CB_MAIN, 0, "cbFSR3Upscaler" },
    { 5, FSR3_BIND_SAMPLER, LINEAR_CLAMP, 0, "s_LinearClamp" },
    { 8, FSR3_BIND_SRV, R(INPUT_EXPOSURE), 0, "r_input_exposure" },
    { 9, FSR3_BIND_SRV, R(DILATED_REACTIVE_MASKS), 1, "r_dilated_reactive_masks" },
    { 10, FSR3_BIND_SRV, R(DILATED_MOTION_VECTORS), 0, "r_dilated_motion_vectors" },
    { 11, FSR3_BIND_SRV, R(INTERNAL_UPSCALED_COLOR), 0, "r_internal_upscaled_color" },
    { 13, FSR3_BIND_SRV, R(FARTHEST_DEPTH_MIP1), 1, "r_farthest_depth_mip1" },
    { 15, FSR3_BIND_SRV, R(LUMA_INSTABILITY), 1, "r_luma_instability" },
    { 16, FSR3_BIND_SRV, R(INPUT_COLOR), 0, "r_input_color_jittered" },
    { 40, FSR3_BIND_UAV, R(INTERNAL_UPSCALED_COLOR), 0, "rw_internal_upscaled_color" },
    { 41, FSR3_BIND_UAV, R(UPSCALED_OUTPUT), 0, "rw_upscaled_output" },
    { 42, FSR3_BIND_UAV, R(NEW_LOCKS), 0, "rw_new_locks" },
};
static const Fsr3Binding rcas[] = {
    { 1, FSR3_BIND_CBV, CB_RCAS, 0, "cbRCAS" },
    { 8, FSR3_BIND_SRV, R(INPUT_EXPOSURE), 0, "r_input_exposure" },
    { 9, FSR3_BIND_SRV, R(RCAS_INPUT), 0, "r_rcas_input" },
    { 40, FSR3_BIND_UAV, R(UPSCALED_OUTPUT), 0, "rw_upscaled_output" },
};

#define LAYOUT(label, x, y, table) { label, x, y, (uint32_t)(sizeof(table) / sizeof((table)[0])), table }
const Fsr3PassLayout fsr3_pass_layout[FSR3_PASS_COUNT] = {
    LAYOUT("FSR3-PREPARE-INPUTS", 8, 8, prepare_inputs),
    LAYOUT("FSR3-LUMA-PYRAMID", 256, 1, luma_pyramid),
    LAYOUT("FSR3-SHADING-CHANGE-PYRAMID", 256, 1, shading_change_pyramid),
    LAYOUT("FSR3-SHADING-CHANGE", 8, 8, shading_change),
    LAYOUT("FSR3-PREPARE-REACTIVITY", 8, 8, prepare_reactivity),
    LAYOUT("FSR3-LUMA-INSTABILITY", 8, 8, luma_instability),
    LAYOUT("FSR3-ACCUMULATE", 8, 8, accumulate),
    LAYOUT("FSR3-RCAS", 64, 1, rcas),
};

void fsr3_host_resolve(const Fsr3Host *h, const Fsr3Binding *b, uint32_t *resource, uint32_t *mip) {
    *mip = 0;
    if (b->kind == FSR3_BIND_SRV) {
        *resource = h->srv[b->id];
    } else if (b->id >= R(SPD_MIPS_LEVEL_0) && b->id <= R(SPD_MIPS_LEVEL_5)) {
        *resource = h->uav[R(SPD_MIPS)];
        *mip = b->id - R(SPD_MIPS_LEVEL_0);
    } else {
        *resource = h->uav[b->id];
    }
}
