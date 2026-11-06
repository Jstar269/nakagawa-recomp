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
/*
 * fsr3_host.h - the CPU side of AMD's FSR 3.1.6 upscaler (ffx_fsr3upscaler.cpp, fsr3upscalerCreate and
 * fsr3upscalerDispatch) as a plan: the constant buffers, which physical image each resource name
 * means this frame (the ping-pong of accumulation, history, luma and upscaled colour, the aliases of
 * the intermediate image), the clears, discards and dispatches in AMD's order, and their sizes.
 *
 * Pure C, no Vulkan: gpu_sdl3vk/fsr3_pass.c records the plan, and tools/test_fsr3_port.py and the
 * selftest compare it with AMD's own C++ (a private oracle built from the unmodified file).
 *
 * Resource numbers are AMD's FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_* values. A physical resource is
 * named by the identifier it was created under (ACCUMULATION_1, LUMA_HISTORY_2, ...); the caller's
 * images keep their input identifiers (INPUT_COLOR, UPSCALED_OUTPUT, ...).
 */
#ifndef SR_FSR3_HOST_H
#define SR_FSR3_HOST_H

#include <stdint.h>

#define FSR3_HOST_RESOURCE_COUNT 60u /* FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_COUNT */

/* FfxFsr3UpscalerInitializationFlagBits, the same bits. */
enum {
    FSR3_HOST_ENABLE_HIGH_DYNAMIC_RANGE = 1u << 0,
    FSR3_HOST_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS = 1u << 1,
    FSR3_HOST_ENABLE_MOTION_VECTORS_JITTER_CANCELLATION = 1u << 2,
    FSR3_HOST_ENABLE_DEPTH_INVERTED = 1u << 3,
    FSR3_HOST_ENABLE_DEPTH_INFINITE = 1u << 4,
    FSR3_HOST_ENABLE_AUTO_EXPOSURE = 1u << 5,
};

/* The eight passes this project compiles, in AMD's dispatch order. */
typedef enum {
    FSR3_PASS_PREPARE_INPUTS,
    FSR3_PASS_LUMA_PYRAMID,
    FSR3_PASS_SHADING_CHANGE_PYRAMID,
    FSR3_PASS_SHADING_CHANGE,
    FSR3_PASS_PREPARE_REACTIVITY,
    FSR3_PASS_LUMA_INSTABILITY,
    FSR3_PASS_ACCUMULATE,
    FSR3_PASS_RCAS,
    FSR3_PASS_COUNT
} Fsr3PassId;

/* cbFSR3Upscaler (Fsr3UpscalerConstants in ffx_fsr3upscaler_private.h), 148 bytes, field for field. */
typedef struct {
    int32_t renderSize[2];
    int32_t previousFrameRenderSize[2];
    int32_t upscaleSize[2];
    int32_t previousFrameUpscaleSize[2];
    int32_t maxRenderSize[2];
    int32_t maxUpscaleSize[2];
    float deviceToViewDepth[4];
    float jitterOffset[2];
    float previousFrameJitterOffset[2];
    float motionVectorScale[2];
    float downscaleFactor[2];
    float motionVectorJitterCancellation[2];
    float tanHalfFOV;
    float jitterPhaseCount;
    float deltaTime;
    float deltaPreExposure;
    float viewSpaceToMetersFactor;
    float frameIndex;
    float velocityFactor;
    float reactivenessScale;
    float shadingChangeScale;
    float accumulationAddedPerFrame;
    float minDisocclusionAccumulation;
} Fsr3Constants;

/* cbSPD (Fsr3UpscalerSpdConstants) and cbRCAS (Fsr3UpscalerRcasConstants). */
typedef struct {
    uint32_t mips;
    uint32_t numworkGroups;
    uint32_t workGroupOffset[2];
    uint32_t renderSize[2];
} Fsr3SpdConstants;
typedef struct {
    uint32_t rcasConfig[4];
} Fsr3RcasConstants;

/* FfxFsr3UpscalerContextDescription, the parts the host uses. */
typedef struct {
    uint32_t flags;
    uint32_t max_render_width, max_render_height;
    uint32_t max_upscale_width, max_upscale_height;
} Fsr3HostDesc;

/* FfxFsr3UpscalerDispatchDescription, the parts the host uses. has_* = 0 is AMD's null resource. */
typedef struct {
    float jitter_x, jitter_y;
    float motion_vector_scale_x, motion_vector_scale_y;
    uint32_t render_width, render_height;
    uint32_t upscale_width, upscale_height; /* both 0: the context's maximum */
    int enable_sharpening;
    float sharpness;
    float frame_time_delta_ms;
    float pre_exposure;
    int reset;
    float camera_near, camera_far, camera_fov_angle_vertical;
    float view_space_to_meters_factor;
    int has_exposure, has_reactive, has_transparency_and_composition;
} Fsr3HostDispatch;

typedef enum { FSR3_JOB_CLEAR, FSR3_JOB_DISCARD, FSR3_JOB_DISPATCH } Fsr3JobKind;

typedef struct {
    Fsr3JobKind kind;
    uint32_t resource;   /* CLEAR, DISCARD: the physical resource (all its mips) */
    float value[4];      /* CLEAR: AMD's float clear colour */
    Fsr3PassId pass;     /* DISPATCH */
    uint32_t groups_x, groups_y;
} Fsr3Job;

#define FSR3_HOST_MAX_JOBS 32u

typedef struct {
    Fsr3HostDesc desc;
    int first_execution;
    uint32_t resource_frame_index;
    float previous_jitter_offset[2];
    float pre_exposure, previous_frame_pre_exposure;
    Fsr3Constants constants;
    /* Filled by fsr3_host_dispatch for the frame being recorded: */
    Fsr3SpdConstants spd;
    Fsr3RcasConstants rcas;
    uint32_t srv[FSR3_HOST_RESOURCE_COUNT]; /* resource name -> physical resource, for reads */
    uint32_t uav[FSR3_HOST_RESOURCE_COUNT]; /* resource name -> physical resource, for writes */
    Fsr3Job jobs[FSR3_HOST_MAX_JOBS];
    uint32_t job_count;
} Fsr3Host;

/* fsr3upscalerCreate's state. */
void fsr3_host_init(Fsr3Host *h, const Fsr3HostDesc *desc);

/* fsr3upscalerDispatch's CPU work for one frame. 0, or -1 for sizes of zero or above the maximum. */
int fsr3_host_dispatch(Fsr3Host *h, const Fsr3HostDispatch *d);

/* One descriptor in a pass's set 0, as the project's SPIR-V declares it (binding number and name),
 * with what AMD's binding tables resolve that name to. */
typedef enum { FSR3_BIND_CBV, FSR3_BIND_SAMPLER, FSR3_BIND_SRV, FSR3_BIND_UAV } Fsr3BindKind;
typedef struct {
    uint32_t binding;
    Fsr3BindKind kind;
    uint32_t id; /* SRV, UAV: AMD resource id; CBV: AMD constant buffer id; SAMPLER: AMD's sampler index */
    int linear;  /* SRV: the shader samples it with s_LinearClamp, so its format needs linear filtering */
    const char *name;
} Fsr3Binding;

typedef struct {
    const char *label;         /* AMD's pipeline name */
    uint32_t local_x, local_y; /* numthreads */
    uint32_t count;
    const Fsr3Binding *bindings;
} Fsr3PassLayout;

extern const Fsr3PassLayout fsr3_pass_layout[FSR3_PASS_COUNT];

/* scheduleDispatch's lookup: the physical resource, and for a UAV the mip level, that an SRV or UAV
 * binding uses in the frame fsr3_host_dispatch last planned. A UAV naming an SPD mip level is the
 * SPD mip chain at that level. */
void fsr3_host_resolve(const Fsr3Host *h, const Fsr3Binding *b, uint32_t *resource, uint32_t *mip);

/* ffxSpdSetup with mips = -1. */
void fsr3_spd_setup(uint32_t dispatch_groups[2], uint32_t work_group_offset[2],
                    uint32_t num_work_groups_and_mips[2], const uint32_t rect[4]);

/* FsrRcasCon (ffx_fsr1.h) and ffxF32ToF16 (ffx_core_cpu.h): con.x the bits of exp2(-sharpness),
 * con.y the half pair, truncating as AMD's table does. */
void fsr3_rcas_con(uint32_t con[4], float sharpness);
uint32_t fsr3_f32_to_f16(float f);

#endif /* SR_FSR3_HOST_H */
