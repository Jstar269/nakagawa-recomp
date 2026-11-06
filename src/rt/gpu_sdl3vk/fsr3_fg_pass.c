/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * Derived from the AMD FidelityFX SDK v2.4.0 (commit 7539ae8b6568ad29ed78a753404351daf3b5cca7), under the
 * MIT licence below, from these files under Kits/FidelityFX/:
 *   framegeneration/fsr3/internal/ffx_frameinterpolation.cpp (getInternalResourceDescriptions,
 *     getSharedResourceDescriptions: frame interpolation's images);
 *   framegeneration/fsr3/internal/ffx_opticalflow.cpp (getInternalResourceDescriptions,
 *     getSharedResourceDescriptions, GetOpticalFlowTextureSize, GetSCDHistogramTextureWidth: optical
 *     flow's images);
 *   framegeneration/fsr3/internal/ffx_provider_fsr3framegeneration.cpp (the shared images, two sets of
 *     frame interpolation's);
 *   backend/vk/ffx_vk.cpp (the full mip chain of a mip count of 0, the storage mip clamp, the clear
 *     colour, and the samplers).
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
 * Modified by Nakagawa Recomp contributors, 2026-11-05: translated from AMD's C++ to C, recording the
 * plan of fsr3_fg_host.c into a Vulkan command buffer.
 */
/* fsr3_fg_pass.c - see fsr3_fg_pass.h. */
#include "fsr3_fg_pass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FFX_CPU 1
#include "frameinterpolation/ffx_frameinterpolation_resources.h"
#include "opticalflow/ffx_opticalflow_resources.h"

static const uint32_t k_spv_reconstruct_and_dilate[] =
#include "fsr3fg_fi_reconstruct_and_dilate_comp.inc"
;
static const uint32_t k_spv_setup[] =
#include "fsr3fg_fi_setup_comp.inc"
;
static const uint32_t k_spv_reconstruct_previous_depth[] =
#include "fsr3fg_fi_reconstruct_previous_depth_comp.inc"
;
static const uint32_t k_spv_game_motion_vector_field[] =
#include "fsr3fg_fi_game_motion_vector_field_comp.inc"
;
static const uint32_t k_spv_optical_flow_vector_field[] =
#include "fsr3fg_fi_optical_flow_vector_field_comp.inc"
;
static const uint32_t k_spv_disocclusion_mask[] =
#include "fsr3fg_fi_disocclusion_mask_comp.inc"
;
static const uint32_t k_spv_interpolation[] =
#include "fsr3fg_fi_interpolation_comp.inc"
;
static const uint32_t k_spv_inpainting_pyramid[] =
#include "fsr3fg_fi_compute_inpainting_pyramid_comp.inc"
;
static const uint32_t k_spv_inpainting[] =
#include "fsr3fg_fi_inpainting_comp.inc"
;
static const uint32_t k_spv_game_vector_field_inpainting_pyramid[] =
#include "fsr3fg_fi_compute_game_vector_field_inpainting_pyramid_comp.inc"
;
static const uint32_t k_spv_of_prepare_luma[] =
#include "fsr3fg_of_prepare_luma_comp.inc"
;
static const uint32_t k_spv_of_input_pyramid[] =
#include "fsr3fg_of_compute_luminance_pyramid_comp.inc"
;
static const uint32_t k_spv_of_scd_histogram[] =
#include "fsr3fg_of_generate_scd_histogram_comp.inc"
;
static const uint32_t k_spv_of_scd_divergence[] =
#include "fsr3fg_of_compute_scd_divergence_comp.inc"
;
static const uint32_t k_spv_of_search[] =
#include "fsr3fg_of_compute_optical_flow_advanced_v5_comp.inc"
;
static const uint32_t k_spv_of_filter[] =
#include "fsr3fg_of_filter_optical_flow_v5_comp.inc"
;
static const uint32_t k_spv_of_scale[] =
#include "fsr3fg_of_scale_optical_flow_advanced_v5_comp.inc"
;

static const struct {
    const uint32_t *code;
    size_t bytes;
} k_spv[FSR3FG_PASS_COUNT] = {
    { k_spv_reconstruct_and_dilate, sizeof(k_spv_reconstruct_and_dilate) },
    { k_spv_setup, sizeof(k_spv_setup) },
    { k_spv_reconstruct_previous_depth, sizeof(k_spv_reconstruct_previous_depth) },
    { k_spv_game_motion_vector_field, sizeof(k_spv_game_motion_vector_field) },
    { k_spv_optical_flow_vector_field, sizeof(k_spv_optical_flow_vector_field) },
    { k_spv_disocclusion_mask, sizeof(k_spv_disocclusion_mask) },
    { k_spv_interpolation, sizeof(k_spv_interpolation) },
    { k_spv_inpainting_pyramid, sizeof(k_spv_inpainting_pyramid) },
    { k_spv_inpainting, sizeof(k_spv_inpainting) },
    { k_spv_game_vector_field_inpainting_pyramid, sizeof(k_spv_game_vector_field_inpainting_pyramid) },
    { k_spv_of_prepare_luma, sizeof(k_spv_of_prepare_luma) },
    { k_spv_of_input_pyramid, sizeof(k_spv_of_input_pyramid) },
    { k_spv_of_scd_histogram, sizeof(k_spv_of_scd_histogram) },
    { k_spv_of_scd_divergence, sizeof(k_spv_of_scd_divergence) },
    { k_spv_of_search, sizeof(k_spv_of_search) },
    { k_spv_of_filter, sizeof(k_spv_of_filter) },
    { k_spv_of_scale, sizeof(k_spv_of_scale) },
};

#define FI(x) FFX_FRAMEINTERPOLATION_RESOURCE_IDENTIFIER_##x
#define OFR(x) (FSR3FG_RES_OF + FFX_OF_RESOURCE_IDENTIFIER_##x)
#define PYRAMID_UAV_MIPS 13u     /* rw_inpainting_pyramid0 .. 12 */
#define OF_LEVELS 7u             /* OpticalFlowMaxPyramidLevels */
#define SCD_HISTOGRAM_WIDTH 2304u /* GetSCDHistogramTextureWidth: HistogramBins 256 x HistogramsPerDim 3 squared */
#define MIN_DISPLAY 64u          /* optical flow's smallest input level is the display >> 6 */
#define SUBGROUP_SIZE 32u
#define CALLS 2u                 /* a slot's prepare and dispatch */

/* How often a call dispatches each pass at most: optical flow's search and filter once a level, its
 * scale once a level but the last. A plan that dispatches a pass more often is refused, not recorded. */
static const uint8_t k_uses[FSR3FG_PASS_COUNT] = {
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* frame interpolation */
    1, 1, 1, 1, OF_LEVELS, OF_LEVELS, OF_LEVELS - 1,
};

/* --- the images: AMD's resource descriptions ------------------------------------------------- */

enum {
    SZ_MAX_RENDER,   /* the maximum render size */
    SZ_DISPLAY,      /* the display */
    SZ_HALF_DISPLAY, /* displaySize / 2 */
    SZ_DISPLAY_8,    /* displaySize / 8, rounded down (frame interpolation's optical-flow fields) */
    SZ_INPUT_LEVEL,  /* display >> level (optical flow's luma pyramid) */
    SZ_FLOW_LEVEL,   /* GetOpticalFlowTextureSize, then halved rounding up per level */
    SZ_HISTOGRAM,    /* SCD_HISTOGRAM_WIDTH x 1 */
    SZ_3X1,
    SZ_1X1,
};
#define COLOR VK_FORMAT_UNDEFINED /* the caller's colour format */
#define ST (VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
#define RO (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)

typedef struct {
    uint32_t id;
    VkFormat format;
    uint8_t size, level;
    uint8_t full_chain; /* AMD's mip count 0 */
    uint8_t linear;     /* a pass samples it with s_LinearClamp */
    VkImageUsageFlags usage;
} ImageSpec;

#define LV(base, sz, fmt) \
    { base + 0, fmt, sz, 0, 0, 0, ST }, { base + 1, fmt, sz, 1, 0, 0, ST }, { base + 2, fmt, sz, 2, 0, 0, ST }, \
    { base + 3, fmt, sz, 3, 0, 0, ST }, { base + 4, fmt, sz, 4, 0, 0, ST }, { base + 5, fmt, sz, 5, 0, 0, ST }, \
    { base + 6, fmt, sz, 6, 0, 0, ST }

static const ImageSpec k_images[] = {
    /* frame interpolation's (frameinterpolationCreate) */
    { FI(RECONSTRUCTED_DEPTH_INTERPOLATED_FRAME), VK_FORMAT_R32_UINT, SZ_MAX_RENDER, 0, 0, 0, ST },
    { FI(GAME_MOTION_VECTOR_FIELD_X), VK_FORMAT_R32_UINT, SZ_MAX_RENDER, 0, 0, 0, ST },
    { FI(GAME_MOTION_VECTOR_FIELD_Y), VK_FORMAT_R32_UINT, SZ_MAX_RENDER, 0, 0, 0, ST },
    { FI(INPAINTING_PYRAMID), VK_FORMAT_R16G16B16A16_SFLOAT, SZ_HALF_DISPLAY, 0, 1, 0, ST },
    { FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_X), VK_FORMAT_R32_UINT, SZ_DISPLAY_8, 0, 0, 0, ST },
    { FI(OPTICAL_FLOW_MOTION_VECTOR_FIELD_Y), VK_FORMAT_R32_UINT, SZ_DISPLAY_8, 0, 0, 0, ST },
    /* AMD makes it a UAV; no pass writes it, only the copy, so the colour format needs no storage use. */
    { FI(PREVIOUS_INTERPOLATION_SOURCE), COLOR, SZ_DISPLAY, 0, 0, 1, RO },
    { FI(INPAINTING_MASK), VK_FORMAT_R8_UNORM, SZ_DISPLAY, 0, 0, 0, ST },
    { FI(DISOCCLUSION_MASK), VK_FORMAT_R8G8_UNORM, SZ_MAX_RENDER, 0, 0, 1, ST },
    /* its initial data is { 0, 0 }, the zero every image starts with */
    { FI(DEFAULT_DISTORTION_FIELD), VK_FORMAT_R8G8_UNORM, SZ_1X1, 0, 0, 1, RO },
    /* optical flow's (opticalflowCreate) */
    LV(OFR(OPTICAL_FLOW_INPUT_1), SZ_INPUT_LEVEL, VK_FORMAT_R8_UINT),
    LV(OFR(OPTICAL_FLOW_INPUT_2), SZ_INPUT_LEVEL, VK_FORMAT_R8_UINT),
    LV(OFR(OPTICAL_FLOW_1), SZ_FLOW_LEVEL, VK_FORMAT_R16G16_SINT),
    LV(OFR(OPTICAL_FLOW_2), SZ_FLOW_LEVEL, VK_FORMAT_R16G16_SINT),
    { OFR(OPTICAL_FLOW_SCD_HISTOGRAM), VK_FORMAT_R32_UINT, SZ_HISTOGRAM, 0, 0, 0, ST },
    { OFR(OPTICAL_FLOW_SCD_PREVIOUS_HISTOGRAM), VK_FORMAT_R32_SFLOAT, SZ_HISTOGRAM, 0, 0, 0, ST },
    { OFR(OPTICAL_FLOW_SCD_TEMP), VK_FORMAT_R32_UINT, SZ_3X1, 0, 0, 0, ST },
    /* the provider's shared images (CreateContext) */
    { FSR3FG_RES_OF_VECTOR, VK_FORMAT_R16G16_SINT, SZ_FLOW_LEVEL, 0, 0, 0, ST },
    { FSR3FG_RES_OF_SCD, VK_FORMAT_R32_UINT, SZ_3X1, 0, 0, 0, ST },
    { FSR3FG_RES_DILATED_DEPTH_0, VK_FORMAT_R32_SFLOAT, SZ_MAX_RENDER, 0, 0, 0, ST },
    { FSR3FG_RES_DILATED_DEPTH_1, VK_FORMAT_R32_SFLOAT, SZ_MAX_RENDER, 0, 0, 0, ST },
    { FSR3FG_RES_DILATED_MOTION_VECTORS_0, VK_FORMAT_R16G16_SFLOAT, SZ_MAX_RENDER, 0, 0, 0, ST },
    { FSR3FG_RES_DILATED_MOTION_VECTORS_1, VK_FORMAT_R16G16_SFLOAT, SZ_MAX_RENDER, 0, 0, 0, ST },
    { FSR3FG_RES_RECONSTRUCTED_PREVIOUS_DEPTH_0, VK_FORMAT_R32_UINT, SZ_MAX_RENDER, 0, 0, 0, ST },
    { FSR3FG_RES_RECONSTRUCTED_PREVIOUS_DEPTH_1, VK_FORMAT_R32_UINT, SZ_MAX_RENDER, 0, 0, 0, ST },
};
#define IMAGE_COUNT (sizeof(k_images) / sizeof(k_images[0]))
#define COUNTERS_BYTES 8u /* FI_Counters: a buffer of 8 bytes, stride 4 */

typedef struct {
    const ImageSpec *spec; /* NULL: no image under this id */
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;                    /* every mip */
    VkImageView mip[PYRAMID_UAV_MIPS];   /* a chain only: one level each */
    VkFormat format;
    uint32_t width, height, mips;
} Img;

typedef struct {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule module;
    VkPipeline pipeline;
} Pipe;

/* The caller's images for one call. */
typedef struct {
    VkImageView depth, motion_vectors;
    VkImageLayout input_layout;
    VkImage present_image, hudless_image;
    VkImageView present, hudless, output;
} Views;

struct Fsr3FgPass {
    Fsr3FgPassInit init;
    Fsr3FgDeviceFacts facts;
    VkDevice dev;
    Fsr3FgHost host;
    Fsr3FgHost *saved; /* the context before a call, put back when the call is refused */
    Img img[FSR3FG_RES_END];
    VkBuffer counters;
    VkDeviceMemory counters_memory;
    uint64_t memory_bytes;
    int initialised; /* the first record zeroes every image and the counters */
    VkSampler samplers[2]; /* AMD's sampler indices: 0 point clamp, 1 linear clamp */
    Pipe pipe[FSR3FG_PASS_COUNT];
    VkDescriptorPool pool;
    VkDescriptorSet *sets;       /* [slot][set_base[pass] + use] */
    uint32_t set_base[FSR3FG_PASS_COUNT], sets_per_slot;
    VkDescriptorSet job_set[FSR3FG_MAX_JOBS]; /* the set each dispatch of the current plan binds */
    VkBuffer constants;
    VkDeviceMemory constants_memory;
    unsigned char *constants_map;
    VkDeviceSize constants_stride; /* one constant region, aligned */

    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
#define FG_FN(name) PFN_vk##name name
    FG_FN(CreateImage); FG_FN(DestroyImage); FG_FN(GetImageMemoryRequirements);
    FG_FN(AllocateMemory); FG_FN(FreeMemory); FG_FN(BindImageMemory); FG_FN(BindBufferMemory);
    FG_FN(MapMemory); FG_FN(UnmapMemory);
    FG_FN(CreateImageView); FG_FN(DestroyImageView); FG_FN(CreateBuffer); FG_FN(DestroyBuffer);
    FG_FN(GetBufferMemoryRequirements); FG_FN(CreateSampler); FG_FN(DestroySampler);
    FG_FN(CreateDescriptorSetLayout); FG_FN(DestroyDescriptorSetLayout);
    FG_FN(CreateDescriptorPool); FG_FN(DestroyDescriptorPool); FG_FN(AllocateDescriptorSets);
    FG_FN(UpdateDescriptorSets); FG_FN(CreatePipelineLayout); FG_FN(DestroyPipelineLayout);
    FG_FN(CreateShaderModule); FG_FN(DestroyShaderModule); FG_FN(CreateComputePipelines);
    FG_FN(DestroyPipeline); FG_FN(DeviceWaitIdle);
    FG_FN(CmdPipelineBarrier); FG_FN(CmdClearColorImage); FG_FN(CmdFillBuffer); FG_FN(CmdCopyImage);
    FG_FN(CmdBindPipeline); FG_FN(CmdBindDescriptorSets); FG_FN(CmdDispatch);
#undef FG_FN
};

static void set_error(char *error, size_t cap, const char *text) {
    if (error && cap) snprintf(error, cap, "%s", text);
}

/* --- the device probe ------------------------------------------------------------------------- */

/* The descriptor kinds as the facts count them. */
enum { DK_UNIFORM, DK_SAMPLER, DK_SAMPLED, DK_STORAGE_IMAGE, DK_STORAGE_BUFFER, DK_COUNT };

static int descriptor_kind(Fsr3FgBindKind kind) {
    switch (kind) {
    case FSR3FG_BIND_CBV: return DK_UNIFORM;
    case FSR3FG_BIND_SAMPLER: return DK_SAMPLER;
    case FSR3FG_BIND_SRV: return DK_SAMPLED;
    case FSR3FG_BIND_UAV: return DK_STORAGE_IMAGE;
    default: return DK_STORAGE_BUFFER;
    }
}

static const VkDescriptorType k_types[DK_COUNT] = {
    VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_DESCRIPTOR_TYPE_SAMPLER, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
    VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
};

static VkDescriptorType descriptor_type(Fsr3FgBindKind kind) {
    return k_types[descriptor_kind(kind)];
}

static int has_device_extension(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice pd,
                                const char *name) {
    PFN_vkEnumerateDeviceExtensionProperties enumerate =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(instance, "vkEnumerateDeviceExtensionProperties");
    uint32_t n = 0;
    if (!enumerate || enumerate(pd, NULL, &n, NULL) != VK_SUCCESS || n == 0) return 0;
    VkExtensionProperties *ep = calloc(n, sizeof(*ep));
    int found = 0;
    if (ep && enumerate(pd, NULL, &n, ep) >= VK_SUCCESS)
        for (uint32_t i = 0; i < n && !found; i++) found = !strcmp(ep[i].extensionName, name);
    free(ep);
    return found;
}

void fsr3_fg_pass_query(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice pd,
                        VkFormat color_format, Fsr3FgDeviceFacts *f) {
    memset(f, 0, sizeof(*f));
    PFN_vkGetPhysicalDeviceProperties props_fn =
        (PFN_vkGetPhysicalDeviceProperties)gipa(instance, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties2 props2_fn =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(instance, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceFeatures2 features2_fn =
        (PFN_vkGetPhysicalDeviceFeatures2)gipa(instance, "vkGetPhysicalDeviceFeatures2");
    PFN_vkGetPhysicalDeviceFormatProperties format_fn =
        (PFN_vkGetPhysicalDeviceFormatProperties)gipa(instance, "vkGetPhysicalDeviceFormatProperties");
    if (!props_fn || !props2_fn || !features2_fn || !format_fn) return; /* api_version 0 */

    VkPhysicalDeviceProperties props;
    props_fn(pd, &props);
    f->api_version = props.apiVersion;
    if (f->api_version < VK_API_VERSION_1_1) return;
    f->size_control_extension = has_device_extension(gipa, instance, pd, VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME);
    const int size_control = f->size_control_extension || f->api_version >= VK_API_VERSION_1_3;

    VkPhysicalDeviceSubgroupSizeControlProperties sp;
    memset(&sp, 0, sizeof(sp));
    sp.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES;
    VkPhysicalDeviceSubgroupProperties sub;
    memset(&sub, 0, sizeof(sub));
    sub.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    sub.pNext = size_control ? &sp : NULL;
    VkPhysicalDeviceProperties2 p2;
    memset(&p2, 0, sizeof(p2));
    p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    p2.pNext = &sub;
    props2_fn(pd, &p2);
    f->subgroup_stages = sub.supportedStages;
    f->subgroup_operations = sub.supportedOperations;
    f->min_subgroup_size = sp.minSubgroupSize;
    f->max_subgroup_size = sp.maxSubgroupSize;
    f->required_size_stages = sp.requiredSubgroupSizeStages;
    const VkPhysicalDeviceLimits *lim = &p2.properties.limits;
    f->max_workgroup_invocations = lim->maxComputeWorkGroupInvocations;
    f->max_workgroup_subgroups = sp.maxComputeWorkgroupSubgroups;
    f->max_per_stage[DK_UNIFORM] = lim->maxPerStageDescriptorUniformBuffers;
    f->max_per_stage[DK_SAMPLER] = lim->maxPerStageDescriptorSamplers;
    f->max_per_stage[DK_SAMPLED] = lim->maxPerStageDescriptorSampledImages;
    f->max_per_stage[DK_STORAGE_IMAGE] = lim->maxPerStageDescriptorStorageImages;
    f->max_per_stage[DK_STORAGE_BUFFER] = lim->maxPerStageDescriptorStorageBuffers;
    f->max_per_stage_resources = lim->maxPerStageResources;

    VkPhysicalDeviceSubgroupSizeControlFeatures sf;
    memset(&sf, 0, sizeof(sf));
    sf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
    VkPhysicalDeviceFeatures2 f2;
    memset(&f2, 0, sizeof(f2));
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = size_control ? &sf : NULL;
    features2_fn(pd, &f2);
    f->storage_read_without_format = f2.features.shaderStorageImageReadWithoutFormat != VK_FALSE;
    f->storage_write_without_format = f2.features.shaderStorageImageWriteWithoutFormat != VK_FALSE;
    f->size_control = sf.subgroupSizeControl != VK_FALSE;
    f->full_subgroups = sf.computeFullSubgroups != VK_FALSE;

    /* Each image as it is used: written as a storage image (atomics reach the R32_UINT ones), read as a
     * sampled one, cleared, and filtered where a pass samples it with s_LinearClamp. The colour format
     * is also the output's (written and read by the passes) and the copy's source. */
    for (uint32_t i = 0; i <= IMAGE_COUNT && !f->format_missing[0]; i++) {
        VkFormat format;
        VkFormatFeatureFlags need;
        if (i == IMAGE_COUNT) {
            format = color_format;
            need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                   VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        } else {
            const ImageSpec *s = &k_images[i];
            format = s->format == COLOR ? color_format : s->format;
            need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
            if (s->usage & VK_IMAGE_USAGE_STORAGE_BIT) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
            if (format == VK_FORMAT_R32_UINT) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
            if (s->linear) need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        }
        VkFormatProperties fp;
        memset(&fp, 0, sizeof(fp));
        if (format != VK_FORMAT_UNDEFINED) format_fn(pd, format, &fp);
        if ((fp.optimalTilingFeatures & need) != need)
            snprintf(f->format_missing, sizeof(f->format_missing), "format %d features 0x%x", (int)format,
                     (unsigned)(need & ~fp.optimalTilingFeatures));
    }
}

const char *fsr3_fg_pass_refusal(const Fsr3FgDeviceFacts *f) {
    static char reason[128];
    if (f->api_version < VK_API_VERSION_1_1) return "Vulkan 1.1 (the shaders are SPIR-V 1.3)";
    if (!f->storage_read_without_format) return "shaderStorageImageReadWithoutFormat";
    if (!f->storage_write_without_format) return "shaderStorageImageWriteWithoutFormat";
    const VkSubgroupFeatureFlags ops = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    if (!(f->subgroup_stages & VK_SHADER_STAGE_COMPUTE_BIT) || (f->subgroup_operations & ops) != ops)
        return "subgroup arithmetic in compute shaders";
    if (!f->size_control_extension && f->api_version < VK_API_VERSION_1_3)
        return "subgroup size control (Vulkan 1.3 or VK_EXT_subgroup_size_control)";
    if (!f->size_control) return "the subgroupSizeControl feature";
    if (!f->full_subgroups) return "the computeFullSubgroups feature";
    if (!(f->required_size_stages & VK_SHADER_STAGE_COMPUTE_BIT) || f->min_subgroup_size > SUBGROUP_SIZE ||
        f->max_subgroup_size < SUBGROUP_SIZE) {
        snprintf(reason, sizeof(reason),
                 "a required subgroup size of 32 in compute shaders (it allows %u to %u%s)", f->min_subgroup_size,
                 f->max_subgroup_size, (f->required_size_stages & VK_SHADER_STAGE_COMPUTE_BIT) ? "" : ", not in compute");
        return reason;
    }
    uint32_t invocations = 0, need[DK_COUNT] = { 0 }, resources = 0;
    for (uint32_t p = 0; p < FSR3FG_PASS_COUNT; p++) {
        const Fsr3FgPassLayout *l = &fsr3_fg_pass_layout[p];
        const uint32_t n = l->local[0] * l->local[1] * l->local[2];
        uint32_t count[DK_COUNT] = { 0 }, res = 0;
        invocations = n > invocations ? n : invocations;
        for (uint32_t b = 0; b < l->count; b++) {
            const int k = descriptor_kind(l->bindings[b].kind);
            count[k]++;
            if (k != DK_SAMPLER) res++;
        }
        for (int k = 0; k < DK_COUNT; k++) need[k] = count[k] > need[k] ? count[k] : need[k];
        resources = res > resources ? res : resources;
    }
    if (f->max_workgroup_invocations < invocations) return "256 invocations a compute workgroup";
    if (f->max_workgroup_subgroups < fsr3_fg_pass_layout[FSR3FG_PASS_OF_SEARCH].local[0] / SUBGROUP_SIZE)
        return "two 32-lane subgroups a workgroup";
    static const char *const names[DK_COUNT] = { "uniform buffers", "samplers", "sampled images", "storage images",
                                                 "storage buffers" };
    for (int k = 0; k < DK_COUNT; k++)
        if (f->max_per_stage[k] < need[k]) {
            snprintf(reason, sizeof(reason), "%u %s a stage (it allows %u)", need[k], names[k], f->max_per_stage[k]);
            return reason;
        }
    if (f->max_per_stage_resources < resources) return "the resources a stage binds";
    if (f->format_missing[0]) {
        snprintf(reason, sizeof(reason), "%s", f->format_missing);
        return reason;
    }
    return NULL;
}

/* --- creation --------------------------------------------------------------------------------- */

static uint32_t memory_type(Fsr3FgPass *p, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    p->GetPhysicalDeviceMemoryProperties(p->init.physical_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static VkImageView make_view(Fsr3FgPass *p, VkImage image, VkFormat format, uint32_t base_mip, uint32_t mips) {
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.baseMipLevel = base_mip;
    vi.subresourceRange.levelCount = mips;
    vi.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (p->CreateImageView(p->dev, &vi, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
    return view;
}

static uint32_t chain_length(uint32_t w, uint32_t h) {
    /* 1 + floor(log2(max(w, h))) */
    uint32_t m = w > h ? w : h, n = 0;
    while (m) {
        n++;
        m >>= 1;
    }
    return n;
}

static void image_size(const Fsr3FgHostDesc *d, const ImageSpec *s, uint32_t *w, uint32_t *h) {
    switch (s->size) {
    case SZ_MAX_RENDER: *w = d->max_render_width; *h = d->max_render_height; break;
    case SZ_DISPLAY: *w = d->display_width; *h = d->display_height; break;
    case SZ_HALF_DISPLAY: *w = d->display_width / 2; *h = d->display_height / 2; break;
    case SZ_DISPLAY_8: *w = d->display_width / 8; *h = d->display_height / 8; break;
    case SZ_INPUT_LEVEL: *w = d->display_width >> s->level; *h = d->display_height >> s->level; break;
    case SZ_FLOW_LEVEL: {
        uint32_t size[2];
        fsr3_fg_of_texture_size(d->display_width, d->display_height, size);
        for (uint32_t k = 0; k < s->level; k++) {
            size[0] = (size[0] + 1) / 2;
            size[1] = (size[1] + 1) / 2;
        }
        *w = size[0];
        *h = size[1];
        break;
    }
    case SZ_HISTOGRAM: *w = SCD_HISTOGRAM_WIDTH; *h = 1; break;
    case SZ_3X1: *w = 3; *h = 1; break;
    default: *w = 1; *h = 1; break;
    }
}

static int make_image(Fsr3FgPass *p, const ImageSpec *s) {
    Img *im = &p->img[s->id];
    im->spec = s;
    image_size(&p->init.desc, s, &im->width, &im->height);
    im->format = s->format == COLOR ? p->init.color_format : s->format;
    im->mips = s->full_chain ? chain_length(im->width, im->height) : 1;
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = im->format;
    ii.extent.width = im->width;
    ii.extent.height = im->height;
    ii.extent.depth = 1;
    ii.mipLevels = im->mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = s->usage;
    if (p->CreateImage(p->dev, &ii, NULL, &im->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    p->GetImageMemoryRequirements(p->dev, im->image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(p, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || p->AllocateMemory(p->dev, &mai, NULL, &im->memory) != VK_SUCCESS ||
        p->BindImageMemory(p->dev, im->image, im->memory, 0) != VK_SUCCESS)
        return 0;
    p->memory_bytes += mr.size;
    im->view = make_view(p, im->image, im->format, 0, im->mips);
    if (!im->view) return 0;
    if (im->mips > 1) {
        if (im->mips > PYRAMID_UAV_MIPS) return 0;
        for (uint32_t m = 0; m < im->mips; m++) {
            im->mip[m] = make_view(p, im->image, im->format, m, 1);
            if (!im->mip[m]) return 0;
        }
    }
    return 1;
}

static int make_counters(Fsr3FgPass *p) {
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = COUNTERS_BYTES;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (p->CreateBuffer(p->dev, &bi, NULL, &p->counters) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    p->GetBufferMemoryRequirements(p->dev, p->counters, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(p, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || p->AllocateMemory(p->dev, &mai, NULL, &p->counters_memory) != VK_SUCCESS ||
        p->BindBufferMemory(p->dev, p->counters, p->counters_memory, 0) != VK_SUCCESS)
        return 0;
    p->memory_bytes += mr.size;
    return 1;
}

static int make_pipe(Fsr3FgPass *p, uint32_t pass) {
    const Fsr3FgPassLayout *l = &fsr3_fg_pass_layout[pass];
    Pipe *pp = &p->pipe[pass];
    VkDescriptorSetLayoutBinding b[FSR3FG_MAX_BINDINGS];
    if (l->count > FSR3FG_MAX_BINDINGS) return 0;
    memset(b, 0, sizeof(b));
    for (uint32_t i = 0; i < l->count; i++) {
        b[i].binding = l->bindings[i].binding;
        b[i].descriptorType = descriptor_type(l->bindings[i].kind);
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, l->count, b };
    if (p->CreateDescriptorSetLayout(p->dev, &dci, NULL, &pp->dsl) != VK_SUCCESS) return 0;
    VkPipelineLayoutCreateInfo lci;
    memset(&lci, 0, sizeof(lci));
    lci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    lci.setLayoutCount = 1;
    lci.pSetLayouts = &pp->dsl;
    if (p->CreatePipelineLayout(p->dev, &lci, NULL, &pp->layout) != VK_SUCCESS) return 0;
    VkShaderModuleCreateInfo mi;
    memset(&mi, 0, sizeof(mi));
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = k_spv[pass].bytes;
    mi.pCode = k_spv[pass].code;
    if (p->CreateShaderModule(p->dev, &mi, NULL, &pp->module) != VK_SUCCESS) return 0;
    /* The search's wave sums are written for 32 lanes, all of them active. */
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rs;
    memset(&rs, 0, sizeof(rs));
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO;
    rs.requiredSubgroupSize = SUBGROUP_SIZE;
    VkComputePipelineCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = pp->module;
    ci.stage.pName = "CS";
    if (l->subgroup_32) {
        ci.stage.pNext = &rs;
        ci.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    }
    ci.layout = pp->layout;
    return p->CreateComputePipelines(p->dev, VK_NULL_HANDLE, 1, &ci, NULL, &pp->pipeline) == VK_SUCCESS;
}

static int make_constants(Fsr3FgPass *p) {
    VkPhysicalDeviceProperties props;
    p->GetPhysicalDeviceProperties(p->init.physical_device, &props);
    const VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment ? props.limits.minUniformBufferOffsetAlignment : 1;
    const VkDeviceSize bytes = FSR3FG_STAGE_WORDS * 4u;
    p->constants_stride = (bytes + align - 1) / align * align;
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = p->constants_stride * FSR3FG_MAX_STAGES * CALLS * p->init.frames_in_flight;
    bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    if (p->CreateBuffer(p->dev, &bi, NULL, &p->constants) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    p->GetBufferMemoryRequirements(p->dev, p->constants, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(p, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    void *map = NULL;
    if (mai.memoryTypeIndex == UINT32_MAX || p->AllocateMemory(p->dev, &mai, NULL, &p->constants_memory) != VK_SUCCESS ||
        p->BindBufferMemory(p->dev, p->constants, p->constants_memory, 0) != VK_SUCCESS ||
        p->MapMemory(p->dev, p->constants_memory, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS)
        return 0;
    p->constants_map = map;
    memset(p->constants_map, 0, (size_t)bi.size);
    return 1;
}

static int make_sets(Fsr3FgPass *p) {
    const uint32_t slots = p->init.frames_in_flight;
    VkDescriptorPoolSize sizes[DK_COUNT];
    for (int k = 0; k < DK_COUNT; k++) {
        sizes[k].type = k_types[k];
        sizes[k].descriptorCount = 0;
    }
    p->sets_per_slot = 0;
    for (uint32_t pass = 0; pass < FSR3FG_PASS_COUNT; pass++) {
        p->set_base[pass] = p->sets_per_slot;
        p->sets_per_slot += k_uses[pass];
        for (uint32_t i = 0; i < fsr3_fg_pass_layout[pass].count; i++)
            sizes[descriptor_kind(fsr3_fg_pass_layout[pass].bindings[i].kind)].descriptorCount += k_uses[pass] * slots;
    }
    VkDescriptorPoolCreateInfo pci;
    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = slots * p->sets_per_slot;
    pci.poolSizeCount = DK_COUNT;
    pci.pPoolSizes = sizes;
    if (p->CreateDescriptorPool(p->dev, &pci, NULL, &p->pool) != VK_SUCCESS) return 0;
    p->sets = calloc((size_t)slots * p->sets_per_slot, sizeof(*p->sets));
    if (!p->sets) return 0;
    for (uint32_t s = 0; s < slots; s++)
        for (uint32_t pass = 0; pass < FSR3FG_PASS_COUNT; pass++)
            for (uint32_t u = 0; u < k_uses[pass]; u++) {
                VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, p->pool, 1,
                                                   &p->pipe[pass].dsl };
                if (p->AllocateDescriptorSets(p->dev, &ai, &p->sets[s * p->sets_per_slot + p->set_base[pass] + u]) !=
                    VK_SUCCESS)
                    return 0;
            }
    return 1;
}

static int make_samplers(Fsr3FgPass *p) {
    for (int k = 0; k < 2; k++) {
        VkSamplerCreateInfo si;
        memset(&si, 0, sizeof(si));
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = si.minFilter = k ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        si.mipmapMode = k ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.minLod = 0.0f;
        si.maxLod = VK_LOD_CLAMP_NONE;
        si.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
        if (p->CreateSampler(p->dev, &si, NULL, &p->samplers[k]) != VK_SUCCESS) return 0;
    }
    return 1;
}

Fsr3FgPass *fsr3_fg_pass_create(const Fsr3FgPassInit *init, char *error, size_t error_size) {
    if (!init->get_instance_proc_addr || !init->device || !init->frames_in_flight || !init->facts) {
        set_error(error, error_size, "FSR3 FG: incomplete init");
        return NULL;
    }
    /* A refused device gets its reason and nothing else: no object is made before this. */
    const char *refusal = fsr3_fg_pass_refusal(init->facts);
    if (refusal) {
        if (error && error_size) snprintf(error, error_size, "%s%s", FSR3FG_PASS_REFUSED_PREFIX, refusal);
        return NULL;
    }
    if (!init->features_enabled) {
        if (error && error_size)
            snprintf(error, error_size, "%sthe device was created without the features it needs",
                     FSR3FG_PASS_REFUSED_PREFIX);
        return NULL;
    }
    const Fsr3FgHostDesc *d = &init->desc;
    if (init->color_format == VK_FORMAT_UNDEFINED) {
        set_error(error, error_size, "FSR3 FG: no colour format");
        return NULL;
    }
    if (d->display_width < MIN_DISPLAY || d->display_height < MIN_DISPLAY) {
        set_error(error, error_size, "FSR3 FG: the display is smaller than 64 x 64");
        return NULL;
    }
    Fsr3FgPass *p = calloc(1, sizeof(*p));
    if (p) p->saved = malloc(sizeof(*p->saved));
    if (!p || !p->saved) {
        if (p) free(p);
        set_error(error, error_size, "FSR3 FG: out of memory");
        return NULL;
    }
    if (fsr3_fg_host_init(&p->host, d) != 0) {
        set_error(error, error_size, "FSR3 FG: the shaders are built for inverted depth, with no other flag but "
                                     "infinite depth and HDR colour, and sizes above 0");
        free(p->saved);
        free(p);
        return NULL;
    }
    p->init = *init;
    p->facts = *init->facts;
    p->init.facts = &p->facts;
    p->dev = init->device;
    PFN_vkGetDeviceProcAddr dev_addr =
        (PFN_vkGetDeviceProcAddr)init->get_instance_proc_addr(init->instance, "vkGetDeviceProcAddr");
    p->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)init->get_instance_proc_addr(
        init->instance, "vkGetPhysicalDeviceMemoryProperties");
    p->GetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)init->get_instance_proc_addr(init->instance, "vkGetPhysicalDeviceProperties");
    if (!dev_addr || !p->GetPhysicalDeviceMemoryProperties || !p->GetPhysicalDeviceProperties) {
        set_error(error, error_size, "FSR3 FG: no Vulkan instance functions");
        free(p->saved);
        free(p);
        return NULL;
    }
    int ok = 1;
#define DEV(name) ok &= (p->name = (PFN_vk##name)dev_addr(p->dev, "vk" #name)) != NULL
    DEV(CreateImage); DEV(DestroyImage); DEV(GetImageMemoryRequirements);
    DEV(AllocateMemory); DEV(FreeMemory); DEV(BindImageMemory); DEV(BindBufferMemory);
    DEV(MapMemory); DEV(UnmapMemory);
    DEV(CreateImageView); DEV(DestroyImageView); DEV(CreateBuffer); DEV(DestroyBuffer);
    DEV(GetBufferMemoryRequirements); DEV(CreateSampler); DEV(DestroySampler);
    DEV(CreateDescriptorSetLayout); DEV(DestroyDescriptorSetLayout);
    DEV(CreateDescriptorPool); DEV(DestroyDescriptorPool); DEV(AllocateDescriptorSets);
    DEV(UpdateDescriptorSets); DEV(CreatePipelineLayout); DEV(DestroyPipelineLayout);
    DEV(CreateShaderModule); DEV(DestroyShaderModule); DEV(CreateComputePipelines);
    DEV(DestroyPipeline); DEV(DeviceWaitIdle);
    DEV(CmdPipelineBarrier); DEV(CmdClearColorImage); DEV(CmdFillBuffer); DEV(CmdCopyImage);
    DEV(CmdBindPipeline); DEV(CmdBindDescriptorSets); DEV(CmdDispatch);
#undef DEV
    if (!ok) {
        set_error(error, error_size, "FSR3 FG: a Vulkan device function is missing");
        free(p->saved);
        free(p);
        return NULL;
    }
    for (uint32_t i = 0; ok && i < IMAGE_COUNT; i++) ok = make_image(p, &k_images[i]);
    if (!ok || !make_counters(p)) {
        set_error(error, error_size, "FSR3 FG: cannot create an internal image or the counters");
        fsr3_fg_pass_destroy(p);
        return NULL;
    }
    if (!make_samplers(p)) {
        set_error(error, error_size, "FSR3 FG: cannot create the samplers");
        fsr3_fg_pass_destroy(p);
        return NULL;
    }
    for (uint32_t pass = 0; ok && pass < FSR3FG_PASS_COUNT; pass++) ok = make_pipe(p, pass);
    if (!ok) {
        set_error(error, error_size, "FSR3 FG: cannot create a pipeline");
        fsr3_fg_pass_destroy(p);
        return NULL;
    }
    if (!make_constants(p) || !make_sets(p)) {
        set_error(error, error_size, "FSR3 FG: cannot create the constants or descriptor sets");
        fsr3_fg_pass_destroy(p);
        return NULL;
    }
    return p;
}

void fsr3_fg_pass_destroy(Fsr3FgPass *p) {
    if (!p) return;
    p->DeviceWaitIdle(p->dev);
    if (p->constants_map) p->UnmapMemory(p->dev, p->constants_memory);
    if (p->constants) p->DestroyBuffer(p->dev, p->constants, NULL);
    if (p->constants_memory) p->FreeMemory(p->dev, p->constants_memory, NULL);
    if (p->pool) p->DestroyDescriptorPool(p->dev, p->pool, NULL);
    free(p->sets);
    for (uint32_t pass = 0; pass < FSR3FG_PASS_COUNT; pass++) {
        Pipe *pp = &p->pipe[pass];
        if (pp->pipeline) p->DestroyPipeline(p->dev, pp->pipeline, NULL);
        if (pp->module) p->DestroyShaderModule(p->dev, pp->module, NULL);
        if (pp->layout) p->DestroyPipelineLayout(p->dev, pp->layout, NULL);
        if (pp->dsl) p->DestroyDescriptorSetLayout(p->dev, pp->dsl, NULL);
    }
    for (int k = 0; k < 2; k++)
        if (p->samplers[k]) p->DestroySampler(p->dev, p->samplers[k], NULL);
    if (p->counters) p->DestroyBuffer(p->dev, p->counters, NULL);
    if (p->counters_memory) p->FreeMemory(p->dev, p->counters_memory, NULL);
    for (uint32_t r = 0; r < FSR3FG_RES_END; r++) {
        Img *im = &p->img[r];
        for (uint32_t m = 0; m < PYRAMID_UAV_MIPS; m++)
            if (im->mip[m]) p->DestroyImageView(p->dev, im->mip[m], NULL);
        if (im->view) p->DestroyImageView(p->dev, im->view, NULL);
        if (im->image) p->DestroyImage(p->dev, im->image, NULL);
        if (im->memory) p->FreeMemory(p->dev, im->memory, NULL);
    }
    free(p->saved);
    free(p);
}

uint64_t fsr3_fg_pass_memory_bytes(const Fsr3FgPass *p) {
    return p ? p->memory_bytes : 0;
}

const Fsr3FgHost *fsr3_fg_pass_host(const Fsr3FgPass *p) {
    return &p->host;
}

void fsr3_fg_pass_configure(Fsr3FgPass *p, uint64_t frame_id, int has_hudless) {
    fsr3_fg_host_configure(&p->host, frame_id, has_hudless);
}

/* --- recording -------------------------------------------------------------------------------- */

#define ALL_STAGES (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT)
#define ALL_WRITES (VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
#define ALL_ACCESSES (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT | \
                      VK_ACCESS_TRANSFER_WRITE_BIT)

/* Every earlier compute or transfer write, before every later compute or transfer access. */
static void barrier_all(Fsr3FgPass *p, VkCommandBuffer cmd) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, ALL_WRITES, ALL_ACCESSES };
    p->CmdPipelineBarrier(cmd, ALL_STAGES, ALL_STAGES, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* The image's contents become undefined: a transition from UNDEFINED, after its earlier uses. */
static void transition_from_undefined(Fsr3FgPass *p, VkCommandBuffer cmd, const Img *im) {
    VkImageMemoryBarrier ib;
    memset(&ib, 0, sizeof(ib));
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.srcAccessMask = ALL_WRITES;
    ib.dstAccessMask = ALL_ACCESSES;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = im->image;
    ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ib.subresourceRange.levelCount = im->mips;
    ib.subresourceRange.layerCount = 1;
    p->CmdPipelineBarrier(cmd, ALL_STAGES, ALL_STAGES, 0, 0, NULL, 0, NULL, 1, &ib);
}

/* AMD's Vulkan backend writes the clear colour's floats into the union for every format, so an integer
 * image takes the floats' bits; the plan carries those bits, and they go in as they are. */
static void clear(Fsr3FgPass *p, VkCommandBuffer cmd, const Img *im, const uint32_t value[4]) {
    VkClearColorValue v;
    memcpy(v.uint32, value, sizeof(v.uint32));
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, im->mips, 0, 1 };
    p->CmdClearColorImage(cmd, im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
}

static const Img *internal(const Fsr3FgPass *p, uint32_t res) {
    return res < FSR3FG_RES_END && p->img[res].image ? &p->img[res] : NULL;
}

/* The view and layout a resolved image binding uses: an internal image, the level of a chain a storage
 * binding names (clamped to the last, as AMD's backend does), or one of the caller's. 0 when it
 * resolves to nothing the pass may bind there. */
static int image_binding(const Fsr3FgPass *p, const Views *v, const Fsr3FgBinding *b, uint32_t res, uint32_t mip,
                         VkDescriptorImageInfo *out) {
    const int storage = b->kind == FSR3FG_BIND_UAV;
    out->sampler = VK_NULL_HANDLE;
    out->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    out->imageView = VK_NULL_HANDLE;
    switch (res) {
    case FSR3FG_RES_DEPTH: out->imageView = storage ? VK_NULL_HANDLE : v->depth; out->imageLayout = v->input_layout; break;
    case FSR3FG_RES_MOTION_VECTORS:
        out->imageView = storage ? VK_NULL_HANDLE : v->motion_vectors;
        out->imageLayout = v->input_layout;
        break;
    case FSR3FG_RES_PRESENT_COLOR: out->imageView = storage ? VK_NULL_HANDLE : v->present; break;
    case FSR3FG_RES_HUDLESS_COLOR: out->imageView = storage ? VK_NULL_HANDLE : v->hudless; break;
    case FSR3FG_RES_OUTPUT: out->imageView = v->output; break;
    default: {
        const Img *im = internal(p, res);
        if (!im) return 0;
        if (storage && !(im->spec->usage & VK_IMAGE_USAGE_STORAGE_BIT)) return 0;
        if (b->linear && !im->spec->linear) return 0; /* its format was not checked for filtering */
        if (storage && im->mips > 1)
            out->imageView = im->mip[mip < im->mips ? mip : im->mips - 1];
        else
            out->imageView = im->view;
        break;
    }
    }
    return out->imageView != VK_NULL_HANDLE;
}

static VkDeviceSize region(const Fsr3FgPass *p, uint32_t slot, uint32_t call, uint32_t stage) {
    return (((VkDeviceSize)slot * CALLS + call) * FSR3FG_MAX_STAGES + stage) * p->constants_stride;
}

/* Writes the slot's descriptor sets for the plan the host just made, and checks every job's resources.
 * 0 when a job cannot be recorded as planned; nothing is then recorded. */
static int write_sets(Fsr3FgPass *p, uint32_t slot, uint32_t call, const Views *v) {
    const Fsr3FgHost *h = &p->host;
    uint32_t uses[FSR3FG_PASS_COUNT];
    memset(uses, 0, sizeof(uses));
    for (uint32_t i = 0; i < h->job_count; i++) {
        const Fsr3FgJob *j = &h->jobs[i];
        switch (j->kind) {
        case FSR3FG_JOB_STAGE:
            if (j->stage >= h->stage_count || h->stages[j->stage].count > FSR3FG_STAGE_WORDS) return 0;
            continue;
        case FSR3FG_JOB_BARRIER: continue;
        case FSR3FG_JOB_CLEAR:
        case FSR3FG_JOB_DISCARD:
            if (!internal(p, j->resource)) return 0;
            continue;
        case FSR3FG_JOB_COPY: {
            const Img *dst = internal(p, j->dest);
            const VkImage src = j->resource == FSR3FG_RES_PRESENT_COLOR   ? v->present_image
                                : j->resource == FSR3FG_RES_HUDLESS_COLOR ? v->hudless_image
                                                                          : VK_NULL_HANDLE;
            if (!src || !dst || dst->format != p->init.color_format || dst->width != p->init.desc.display_width ||
                dst->height != p->init.desc.display_height)
                return 0;
            continue;
        }
        case FSR3FG_JOB_DISPATCH: break;
        default: return 0;
        }
        if ((uint32_t)j->pass >= FSR3FG_PASS_COUNT || uses[j->pass] >= k_uses[j->pass]) return 0;
        const VkDescriptorSet set = p->sets[slot * p->sets_per_slot + p->set_base[j->pass] + uses[j->pass]++];
        p->job_set[i] = set;
        const Fsr3FgPassLayout *l = &fsr3_fg_pass_layout[j->pass];
        VkWriteDescriptorSet w[FSR3FG_MAX_BINDINGS];
        VkDescriptorImageInfo ii[FSR3FG_MAX_BINDINGS];
        VkDescriptorBufferInfo bi[FSR3FG_MAX_BINDINGS];
        for (uint32_t k = 0; k < l->count; k++) {
            const Fsr3FgBinding *b = &l->bindings[k];
            memset(&w[k], 0, sizeof(w[k]));
            w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[k].dstSet = set;
            w[k].dstBinding = b->binding;
            w[k].descriptorCount = 1;
            w[k].descriptorType = descriptor_type(b->kind);
            switch (b->kind) {
            case FSR3FG_BIND_CBV:
                /* the latest region of that buffer staged in this call */
                if (j->res[k] >= h->stage_count || h->stages[j->res[k]].kind != (Fsr3FgStageKind)b->id) return 0;
                bi[k].buffer = p->constants;
                bi[k].offset = region(p, slot, call, j->res[k]);
                bi[k].range = p->constants_stride;
                w[k].pBufferInfo = &bi[k];
                break;
            case FSR3FG_BIND_SRV_BUFFER:
            case FSR3FG_BIND_UAV_BUFFER:
                if (j->res[k] != FI(COUNTERS)) return 0;
                bi[k].buffer = p->counters;
                bi[k].offset = 0;
                bi[k].range = VK_WHOLE_SIZE;
                w[k].pBufferInfo = &bi[k];
                break;
            case FSR3FG_BIND_SAMPLER:
                if (j->res[k] >= 2) return 0;
                memset(&ii[k], 0, sizeof(ii[k]));
                ii[k].sampler = p->samplers[j->res[k]];
                w[k].pImageInfo = &ii[k];
                break;
            default:
                if (!image_binding(p, v, b, j->res[k], j->mip[k], &ii[k])) return 0;
                w[k].pImageInfo = &ii[k];
                break;
            }
        }
        p->UpdateDescriptorSets(p->dev, l->count, w, 0, NULL);
    }
    return 1;
}

static void record(Fsr3FgPass *p, VkCommandBuffer cmd, uint32_t slot, uint32_t call, const Views *v) {
    const Fsr3FgHost *h = &p->host;
    for (uint32_t s = 0; s < h->stage_count; s++)
        memcpy(p->constants_map + region(p, slot, call, s), h->stages[s].words, h->stages[s].count * 4u);

    if (!p->initialised) {
        /* The first record defines every image: GENERAL, and zero, as a D3D12 committed resource starts.
         * AMD's own first-use clears follow in the plan. */
        static const uint32_t zero[4] = { 0, 0, 0, 0 };
        for (uint32_t i = 0; i < IMAGE_COUNT; i++) transition_from_undefined(p, cmd, &p->img[k_images[i].id]);
        for (uint32_t i = 0; i < IMAGE_COUNT; i++) clear(p, cmd, &p->img[k_images[i].id], zero);
        p->CmdFillBuffer(cmd, p->counters, 0, VK_WHOLE_SIZE, 0);
        p->initialised = 1;
    }
    for (uint32_t i = 0; i < h->job_count; i++) {
        const Fsr3FgJob *j = &h->jobs[i];
        switch (j->kind) {
        case FSR3FG_JOB_STAGE:   /* written above, each to its own region */
        case FSR3FG_JOB_BARRIER: /* every access below waits on every earlier write */
            break;
        case FSR3FG_JOB_CLEAR:
            barrier_all(p, cmd);
            clear(p, cmd, &p->img[j->resource], j->value);
            break;
        case FSR3FG_JOB_DISCARD:
            transition_from_undefined(p, cmd, &p->img[j->resource]);
            break;
        case FSR3FG_JOB_COPY: {
            const Img *dst = &p->img[j->dest];
            VkImageCopy c;
            memset(&c, 0, sizeof(c));
            c.srcSubresource.aspectMask = c.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.srcSubresource.layerCount = c.dstSubresource.layerCount = 1;
            c.extent.width = dst->width;
            c.extent.height = dst->height;
            c.extent.depth = 1;
            barrier_all(p, cmd);
            p->CmdCopyImage(cmd, j->resource == FSR3FG_RES_PRESENT_COLOR ? v->present_image : v->hudless_image,
                            VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            break;
        }
        case FSR3FG_JOB_DISPATCH: {
            const Pipe *pp = &p->pipe[j->pass];
            barrier_all(p, cmd);
            p->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pp->pipeline);
            p->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pp->layout, 0, 1, &p->job_set[i], 0, NULL);
            p->CmdDispatch(cmd, j->groups[0], j->groups[1], j->groups[2]);
            break;
        }
        }
    }
}

int fsr3_fg_pass_prepare(Fsr3FgPass *p, VkCommandBuffer cmd, const Fsr3FgPassPrepare *f) {
    if (!p || !cmd || !f || f->slot >= p->init.frames_in_flight || !f->depth || !f->motion_vectors) return -1;
    if (f->input_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && f->input_layout != VK_IMAGE_LAYOUT_GENERAL)
        return -1;
    Views v;
    memset(&v, 0, sizeof(v));
    v.depth = f->depth;
    v.motion_vectors = f->motion_vectors;
    v.input_layout = f->input_layout;
    /* A refused call leaves the context as it was. */
    *p->saved = p->host;
    if (fsr3_fg_host_prepare(&p->host, &f->params) != 0 || !write_sets(p, f->slot, 0, &v)) {
        p->host = *p->saved;
        return -1;
    }
    record(p, cmd, f->slot, 0, &v);
    return 0;
}

int fsr3_fg_pass_dispatch(Fsr3FgPass *p, VkCommandBuffer cmd, const Fsr3FgPassDispatch *f) {
    if (!p || !cmd || !f || f->slot >= p->init.frames_in_flight || !f->present_image || !f->present || !f->output)
        return -1;
    if (!f->hudless != !f->hudless_image || (f->hudless != VK_NULL_HANDLE) != (p->host.has_hudless != 0)) return -1;
    Views v;
    memset(&v, 0, sizeof(v));
    v.input_layout = VK_IMAGE_LAYOUT_GENERAL;
    v.present_image = f->present_image;
    v.present = f->present;
    v.hudless_image = f->hudless_image;
    v.hudless = f->hudless;
    v.output = f->output;
    *p->saved = p->host;
    if (fsr3_fg_host_dispatch(&p->host, &f->params) != 0 || !write_sets(p, f->slot, 1, &v)) {
        p->host = *p->saved;
        return -1;
    }
    record(p, cmd, f->slot, 1, &v);
    return 0;
}
