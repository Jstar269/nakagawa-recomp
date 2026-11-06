/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SPDX-License-Identifier: MIT */
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
/*
 * fsr3_fg_host.h - the CPU side of AMD's FSR 3.1.6 frame generation as a plan: what its provider,
 * frame interpolation (ffx_frameinterpolation.cpp) and optical flow (ffx_opticalflow.cpp) ask the
 * backend to do for one call, in AMD's order: the constant buffers each call stages, the clears,
 * discards, barriers and the copy, and every dispatch with its group counts and the physical image
 * behind each of its bindings.
 *
 * Pure C, no Vulkan: gpu_sdl3vk/fsr3_fg_pass.c records the plan, and tests/native/
 * fsr3_fg_host_selftest.c compares it with a record of AMD's own C++ (built from the unmodified files).
 *
 * The caller drives it as AMD's provider is driven: per real frame, fsr3_fg_host_prepare
 * (ffxDispatchDescFrameGenerationPrepareV2) and fsr3_fg_host_configure (ffxConfigureDescFrameGeneration);
 * for a frame that gets a generated image, fsr3_fg_host_dispatch (ffxDispatchDescFrameGeneration).
 * Each call replaces the plan in jobs[] and stages[].
 *
 * Physical resources are numbered in one space: frame interpolation's internal images under their AMD
 * resource ids (FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_*), optical flow's at FSR3FG_RES_OF + their
 * AMD ids (FFX_OF_RESOURCE_IDENTIFIER_*), and the images the caller or the provider owns at the
 * FSR3FG_RES_* values below. 0 is AMD's null resource.
 */
#ifndef SR_FSR3_FG_HOST_H
#define SR_FSR3_FG_HOST_H

#include <stdint.h>

#define FSR3FG_FI_RESOURCE_COUNT 48u /* FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_COUNT */
#define FSR3FG_OF_RESOURCE_COUNT 32u /* FFX_OF_RESOURCE_IDENTIFIER_COUNT */
#define FSR3FG_OF_BINDING_COUNT 25u  /* FFX_OF_BINDING_IDENTIFIER_COUNT */

/* The physical resources outside frame interpolation's own. */
enum {
    FSR3FG_RES_NULL = 0,
    FSR3FG_RES_OF = 64,         /* + FFX_OF_RESOURCE_IDENTIFIER_*: optical flow's internal images */
    FSR3FG_RES_DEPTH = 128,     /* the game's depth, render size (prepare) */
    FSR3FG_RES_MOTION_VECTORS,  /* the game's motion vectors, render size (prepare) */
    FSR3FG_RES_PRESENT_COLOR,   /* the real frame as presented, with its HUD */
    FSR3FG_RES_HUDLESS_COLOR,   /* the same frame before its HUD, when the caller has it */
    FSR3FG_RES_OUTPUT,          /* the generated image */
    /* The provider's shared resources (ffx_provider_fsr3framegeneration.cpp, CreateContext). */
    FSR3FG_RES_OF_VECTOR,       /* optical flow's vectors, R16G16_SINT, ceil(display / 8) */
    FSR3FG_RES_OF_SCD,          /* optical flow's scene-change result, R32_UINT 3x1 */
    FSR3FG_RES_DILATED_DEPTH_0, /* frame interpolation's prepared inputs, two sets, maximum render size */
    FSR3FG_RES_DILATED_DEPTH_1,
    FSR3FG_RES_DILATED_MOTION_VECTORS_0,
    FSR3FG_RES_DILATED_MOTION_VECTORS_1,
    FSR3FG_RES_RECONSTRUCTED_PREVIOUS_DEPTH_0,
    FSR3FG_RES_RECONSTRUCTED_PREVIOUS_DEPTH_1,
    FSR3FG_RES_END
};

/* FfxFrameInterpolationInitializationFlagBits, the same bits. The project builds one permutation of
 * the shaders (render-size motion vectors, inverted depth, no jitter on the vectors), so
 * fsr3_fg_host_init accepts DEPTH_INVERTED, which it requires, DEPTH_INFINITE and HDR_COLOR_INPUT, and
 * refuses the rest. */
enum {
    FSR3FG_HOST_ENABLE_DEPTH_INVERTED = 1u << 0,
    FSR3FG_HOST_ENABLE_DEPTH_INFINITE = 1u << 1,
    FSR3FG_HOST_ENABLE_HDR_COLOR_INPUT = 1u << 3,
};

/* The seventeen passes, in shader_embed.py's order: frame interpolation's FfxFrameInterpolationPass
 * values 0 to 9, then optical flow's FfxOpticalflowPass values 0 to 6. */
typedef enum {
    FSR3FG_PASS_RECONSTRUCT_AND_DILATE,
    FSR3FG_PASS_SETUP,
    FSR3FG_PASS_RECONSTRUCT_PREVIOUS_DEPTH,
    FSR3FG_PASS_GAME_MOTION_VECTOR_FIELD,
    FSR3FG_PASS_OPTICAL_FLOW_VECTOR_FIELD,
    FSR3FG_PASS_DISOCCLUSION_MASK,
    FSR3FG_PASS_INTERPOLATION,
    FSR3FG_PASS_INPAINTING_PYRAMID,
    FSR3FG_PASS_INPAINTING,
    FSR3FG_PASS_GAME_VECTOR_FIELD_INPAINTING_PYRAMID,
    FSR3FG_PASS_OF_PREPARE_LUMA,
    FSR3FG_PASS_OF_INPUT_PYRAMID,
    FSR3FG_PASS_OF_SCD_HISTOGRAM,
    FSR3FG_PASS_OF_SCD_DIVERGENCE,
    FSR3FG_PASS_OF_SEARCH,
    FSR3FG_PASS_OF_FILTER,
    FSR3FG_PASS_OF_SCALE,
    FSR3FG_PASS_COUNT
} Fsr3FgPassId;

/* cbFI (FrameInterpolationConstants in ffx_frameinterpolation_private.h), 192 bytes, field for field. */
typedef struct {
    int32_t renderSize[2];
    int32_t displaySize[2];
    float displaySizeRcp[2];
    float cameraNear;
    float cameraFar;
    int32_t colorRectSize[2];
    int32_t Mode;
    int32_t Reset;
    float deviceToViewDepth[4];
    float deltaTime;
    int32_t HUDLessAttachedFactor;
    int32_t distortionFieldSize[2];
    float opticalFlowScale[2];
    int32_t opticalFlowBlockSize;
    uint32_t dispatchFlags;
    int32_t maxRenderSize[2];
    int32_t opticalFlowHalfResMode;
    int32_t numInstances;
    int32_t interpolationRectBase[2];
    int32_t interpolationRectSize[2];
    float debugBarColor[3];
    uint32_t backBufferTransferFunction;
    float minMaxLuminance[2];
    float fTanHalfFOV;
    float _pad1;
    float jitter[2];
    float motionVectorScale[2];
    int32_t colorRectBase[2];
    int32_t _pad2[2];
} Fsr3FgConstants;

/* cbInpaintingPyramid (InpaintingPyramidConstants). */
typedef struct {
    uint32_t mips;
    uint32_t numworkGroups;
    uint32_t workGroupOffset[2];
} Fsr3FgInpaintingConstants;

/* cbOF (OpticalflowConstants in ffx_opticalflow_private.h). */
typedef struct {
    int32_t inputLumaResolution[2];
    uint32_t opticalFlowPyramidLevel;
    uint32_t opticalFlowPyramidLevelCount;
    int32_t frameIndex;
    uint32_t backbufferTransferFunction;
    float minMaxLuminance[2];
} Fsr3OfConstants;

/* cbOF_SPD (OpticalFlowSpdConstants in ffx_opticalflow.cpp). AMD stages it every dispatch; the input
 * pyramid pass asks SpdDownsample for 6 mips, which returns before it reads the buffer, so the
 * compiled pass declares no such binding. */
typedef struct {
    uint32_t mips;
    uint32_t numworkGroups;
    uint32_t workGroupOffset[2];
    uint32_t numworkGroupsOpticalFlowInputPyramid;
    uint32_t pad[3];
} Fsr3OfSpdConstants;

/* ffxCreateContextDescFrameGeneration, the parts the host uses. */
typedef struct {
    uint32_t flags;
    uint32_t max_render_width, max_render_height;
    uint32_t display_width, display_height;
} Fsr3FgHostDesc;

/* ffxDispatchDescFrameGenerationPrepareV2, the parts FSR 3.1.6 reads. Its camera vectors are not
 * among them: neither the prepare nor the dispatch reads them. */
typedef struct {
    uint64_t frame_id;
    uint32_t render_width, render_height;
    float jitter_x, jitter_y;
    float motion_vector_scale_x, motion_vector_scale_y;
    float frame_time_delta; /* milliseconds; frame interpolation passes it through unconverted */
    float camera_near, camera_far, camera_fov_angle_vertical;
    float view_space_to_meters_factor;
    int reset;
} Fsr3FgHostPrepare;

/* ffxDispatchDescFrameGeneration, the parts the host uses. The present colour is the display size. */
typedef struct {
    uint64_t frame_id;
    int reset;
    uint32_t generation_rect[4]; /* left, top, width, height; a width and height of 0 is the display */
    uint32_t backbuffer_transfer_function;
    float min_max_luminance[2];
} Fsr3FgHostDispatch;

typedef enum {
    FSR3FG_JOB_STAGE,    /* a constant buffer staged: stages[stage] */
    FSR3FG_JOB_CLEAR,    /* clear `resource` (all its mips) to `value`: AMD's clear colour as bits */
    FSR3FG_JOB_BARRIER,  /* AMD's transition of `resource` to `state` (FfxApiResourceStates) */
    FSR3FG_JOB_DISCARD,  /* `resource`'s contents become undefined */
    FSR3FG_JOB_COPY,     /* copy `resource` to `dest` */
    FSR3FG_JOB_DISPATCH, /* `pass` with `groups`, its bindings resolved in res[] and mip[] */
} Fsr3FgJobKind;

#define FSR3FG_MAX_BINDINGS 20u
#define FSR3FG_MAX_JOBS 160u
#define FSR3FG_MAX_STAGES 16u
#define FSR3FG_STAGE_WORDS 48u

typedef struct {
    Fsr3FgJobKind kind;
    uint32_t resource;
    uint32_t dest;
    uint32_t value[4];
    uint32_t state;
    uint32_t stage;
    Fsr3FgPassId pass;
    uint32_t groups[3];
    /* Per binding of the pass layout, in its order: the physical resource and mip of an image or
     * buffer, the stage index of a constant buffer, AMD's sampler index of a sampler. */
    uint32_t res[FSR3FG_MAX_BINDINGS];
    uint32_t mip[FSR3FG_MAX_BINDINGS];
} Fsr3FgJob;

typedef enum { FSR3FG_STAGE_FI, FSR3FG_STAGE_INPAINTING, FSR3FG_STAGE_OF, FSR3FG_STAGE_OF_SPD } Fsr3FgStageKind;

typedef struct {
    Fsr3FgStageKind kind;
    uint32_t count; /* words */
    uint32_t words[FSR3FG_STAGE_WORDS];
} Fsr3FgStage;

typedef struct {
    Fsr3FgHostDesc desc;

    /* FfxFrameInterpolationContext_Private */
    Fsr3FgConstants fi;
    Fsr3FgInpaintingConstants inpainting;
    uint32_t fi_srv[FSR3FG_FI_RESOURCE_COUNT];
    uint32_t fi_uav[FSR3FG_FI_RESOURCE_COUNT];
    uint64_t fi_previous_frame_id;
    uint64_t fi_dispatch_count;
    uint32_t debug_bar_index; /* AMD's is one function-static index for the whole process */
    /* the render description setupDeviceDepthToViewSpaceDepthParams reads */
    float rd_camera_near, rd_camera_far, rd_camera_fov, rd_meters;
    uint32_t rd_render_width, rd_render_height;

    /* FfxOpticalflowContext_Private */
    Fsr3OfConstants of;
    uint32_t of_srv[FSR3FG_OF_BINDING_COUNT];
    uint32_t of_uav[FSR3FG_OF_BINDING_COUNT];
    int of_first_execution;
    uint32_t of_resource_frame_index;

    /* InternalFgContext (the provider) */
    Fsr3FgHostPrepare prepared[2]; /* prepareDescriptions[frameID % MAX_QUEUED_FRAMES] */
    uint64_t last_frame_id;
    uint64_t last_configure_frame_id;
    int has_hudless;
    uint32_t shared_toggle; /* sharedResoureFrameToggle */

    /* The plan of the last call. */
    Fsr3FgStage stages[FSR3FG_MAX_STAGES];
    uint32_t stage_count;
    Fsr3FgJob jobs[FSR3FG_MAX_JOBS];
    uint32_t job_count;
    int overflow;         /* the call needed more than FSR3FG_MAX_JOBS jobs or FSR3FG_MAX_STAGES stages */
    uint32_t cb_stage[4]; /* the latest stage of each constant buffer in this call, by Fsr3FgStageKind */
} Fsr3FgHost;

/* CreateContext's state. 0, or -1 for flags the project's shaders were not built for or a size of 0. */
int fsr3_fg_host_init(Fsr3FgHost *h, const Fsr3FgHostDesc *desc);

/* ffxConfigureDescFrameGeneration for frame `frame_id`: its frame ID and whether it has a HUD-less colour. */
void fsr3_fg_host_configure(Fsr3FgHost *h, uint64_t frame_id, int has_hudless);

/* The provider's prepare and ffxFrameInterpolationPrepare. 0, or -1 for a render size of 0 or above the
 * maximum, or a plan that does not fit. */
int fsr3_fg_host_prepare(Fsr3FgHost *h, const Fsr3FgHostPrepare *p);

/* The provider's dispatch: optical flow, then frame interpolation, for the frame prepared under
 * d->frame_id. 0, or -1 for a generation rectangle outside the display or a plan that does not fit. */
int fsr3_fg_host_dispatch(Fsr3FgHost *h, const Fsr3FgHostDispatch *d);

/* One descriptor of a pass's set 0, as the project's SPIR-V declares it (binding number and name),
 * with what AMD's binding tables resolve that name to. */
typedef enum {
    FSR3FG_BIND_CBV,        /* id: the Fsr3FgStageKind it reads */
    FSR3FG_BIND_SAMPLER,    /* id: AMD's sampler index (0 point clamp, 1 linear clamp) */
    FSR3FG_BIND_SRV,        /* a sampled image */
    FSR3FG_BIND_UAV,        /* a storage image */
    FSR3FG_BIND_SRV_BUFFER, /* a storage buffer read only (r_counters) */
    FSR3FG_BIND_UAV_BUFFER, /* a storage buffer (rw_counters) */
} Fsr3FgBindKind;

typedef struct {
    uint32_t binding;
    Fsr3FgBindKind kind;
    uint32_t id; /* SRV, UAV, buffers: AMD's identifier in the pass's context (FI resource or OF binding) */
    int linear;  /* SRV: the shader samples it with s_LinearClamp, so its format needs linear filtering */
    const char *name;
} Fsr3FgBinding;

typedef struct {
    const char *label;         /* AMD's pipeline name */
    uint32_t local[3];         /* numthreads */
    int optical_flow;          /* 1: an optical-flow pass, its ids are OF binding identifiers */
    int subgroup_32;           /* 1: the pass uses subgroup operations sized for 32 lanes */
    uint32_t count;
    const Fsr3FgBinding *bindings;
} Fsr3FgPassLayout;

extern const Fsr3FgPassLayout fsr3_fg_pass_layout[FSR3FG_PASS_COUNT];

/* ffxSpdSetup: mips < 0 derives the count from the rectangle, as fsr3_spd_setup does. */
void fsr3_fg_spd_setup(uint32_t dispatch_groups[2], uint32_t work_group_offset[2],
                       uint32_t num_work_groups_and_mips[2], const uint32_t rect[4], int32_t mips);

/* GetOpticalFlowTextureSize with the provider's block size of 8: optical flow's vector image size. */
void fsr3_fg_of_texture_size(uint32_t display_width, uint32_t display_height, uint32_t size[2]);

#endif /* SR_FSR3_FG_HOST_H */
