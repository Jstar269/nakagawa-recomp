/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * Derived from the AMD FidelityFX SDK v2.4.0 (commit 7539ae8b6568ad29ed78a753404351daf3b5cca7):
 * Kits/FidelityFX/upscalers/fsr3/internal/ffx_fsr3upscaler.cpp (getInternalResourceDescriptions,
 * scheduleDispatch) and Kits/FidelityFX/backend/vk/ffx_vk.cpp (the clears, mip counts and sampler),
 * under the MIT licence below.
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
 * Modified by Nakagawa Recomp contributors, 2026-11-05: translated from AMD's C++ backend to C,
 * recording the plan of fsr3_host.c into a Vulkan command buffer.
 */
/* fsr3_pass.c - see fsr3_pass.h. The image table is getInternalResourceDescriptions' and
 * getSharedResourceDescriptions' (ffx_fsr3upscaler.cpp); the mip count of a full chain, the clear
 * over all mips with the float colour, and the linear-clamp sampler are as AMD's Vulkan backend
 * (backend/vk/ffx_vk.cpp) makes them. */
#include "fsr3_pass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FFX_CPU 1
#include "fsr3upscaler/ffx_fsr3upscaler_resources.h"

static const uint32_t k_spv_prepare_inputs[] =
#include "fsr3_prepare_inputs_comp.inc"
;
static const uint32_t k_spv_luma_pyramid[] =
#include "fsr3_luma_pyramid_comp.inc"
;
static const uint32_t k_spv_shading_change_pyramid[] =
#include "fsr3_shading_change_pyramid_comp.inc"
;
static const uint32_t k_spv_shading_change[] =
#include "fsr3_shading_change_comp.inc"
;
static const uint32_t k_spv_prepare_reactivity[] =
#include "fsr3_prepare_reactivity_comp.inc"
;
static const uint32_t k_spv_luma_instability[] =
#include "fsr3_luma_instability_comp.inc"
;
static const uint32_t k_spv_accumulate[] =
#include "fsr3_accumulate_comp.inc"
;
static const uint32_t k_spv_rcas[] =
#include "fsr3_rcas_comp.inc"
;

static const struct {
    const uint32_t *code;
    size_t bytes;
} k_spv[FSR3_PASS_COUNT] = {
    { k_spv_prepare_inputs, sizeof(k_spv_prepare_inputs) },
    { k_spv_luma_pyramid, sizeof(k_spv_luma_pyramid) },
    { k_spv_shading_change_pyramid, sizeof(k_spv_shading_change_pyramid) },
    { k_spv_shading_change, sizeof(k_spv_shading_change) },
    { k_spv_prepare_reactivity, sizeof(k_spv_prepare_reactivity) },
    { k_spv_luma_instability, sizeof(k_spv_luma_instability) },
    { k_spv_accumulate, sizeof(k_spv_accumulate) },
    { k_spv_rcas, sizeof(k_spv_rcas) },
};

#define R(x) FFX_FSR3UPSCALER_RESOURCE_IDENTIFIER_##x
#define RESOURCES FSR3_HOST_RESOURCE_COUNT
#define SPD_UAV_MIPS 6u /* rw_spd_mip0 .. rw_spd_mip5 */

/* The internal images, AMD's table: format, and size from the context's maximum sizes. */
enum { SIZE_RENDER, SIZE_HALF_RENDER, SIZE_UPSCALE, SIZE_ONE };
static const struct {
    uint32_t id;
    VkFormat format;
    int size;
    int full_chain; /* AMD's mipCount 0 */
} k_images[] = {
    { R(ACCUMULATION_1), VK_FORMAT_R8_UNORM, SIZE_RENDER, 0 },
    { R(ACCUMULATION_2), VK_FORMAT_R8_UNORM, SIZE_RENDER, 0 },
    { R(LUMA_1), VK_FORMAT_R16_SFLOAT, SIZE_RENDER, 0 },
    { R(LUMA_2), VK_FORMAT_R16_SFLOAT, SIZE_RENDER, 0 },
    { R(INTERMEDIATE_FP16x1), VK_FORMAT_R16_SFLOAT, SIZE_RENDER, 0 },
    { R(SHADING_CHANGE), VK_FORMAT_R8_UNORM, SIZE_HALF_RENDER, 0 },
    { R(NEW_LOCKS), VK_FORMAT_R8_UNORM, SIZE_UPSCALE, 0 },
    { R(INTERNAL_UPSCALED_COLOR_1), VK_FORMAT_R16G16B16A16_SFLOAT, SIZE_UPSCALE, 0 },
    { R(INTERNAL_UPSCALED_COLOR_2), VK_FORMAT_R16G16B16A16_SFLOAT, SIZE_UPSCALE, 0 },
    { R(SPD_MIPS), VK_FORMAT_R16G16_SFLOAT, SIZE_HALF_RENDER, 1 },
    { R(FARTHEST_DEPTH_MIP1), VK_FORMAT_R16_SFLOAT, SIZE_HALF_RENDER, 0 },
    { R(LUMA_HISTORY_1), VK_FORMAT_R16G16B16A16_SFLOAT, SIZE_RENDER, 0 },
    { R(LUMA_HISTORY_2), VK_FORMAT_R16G16B16A16_SFLOAT, SIZE_RENDER, 0 },
    { R(SPD_ATOMIC_COUNT), VK_FORMAT_R32_UINT, SIZE_ONE, 0 },
    { R(DILATED_REACTIVE_MASKS), VK_FORMAT_R8G8B8A8_UNORM, SIZE_RENDER, 0 },
    { R(INTERNAL_DEFAULT_REACTIVITY), VK_FORMAT_R8_UNORM, SIZE_ONE, 0 },
    { R(INTERNAL_DEFAULT_EXPOSURE), VK_FORMAT_R32G32_SFLOAT, SIZE_ONE, 0 },
    { R(FRAME_INFO), VK_FORMAT_R32G32B32A32_SFLOAT, SIZE_ONE, 0 },
    /* AMD's shared resources, which its caller may own; here they are the pass's. */
    { R(DILATED_DEPTH), VK_FORMAT_R32_SFLOAT, SIZE_RENDER, 0 },
    { R(DILATED_MOTION_VECTORS), VK_FORMAT_R16G16_SFLOAT, SIZE_RENDER, 0 },
    { R(RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH), VK_FORMAT_R32_UINT, SIZE_RENDER, 0 },
};
#define IMAGE_COUNT (sizeof(k_images) / sizeof(k_images[0]))

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;            /* every mip */
    VkImageView mip[SPD_UAV_MIPS]; /* SPD_MIPS only: one level each */
    VkFormat format;
    uint32_t mips;
} Img;

typedef struct {
    VkDescriptorSetLayout dsl;
    VkPipelineLayout layout;
    VkShaderModule module;
    VkPipeline pipeline;
} Pipe;

struct Fsr3Pass {
    Fsr3PassInit init;
    VkDevice dev;
    Fsr3Host host;
    Img img[RESOURCES]; /* by AMD resource id; image is VK_NULL_HANDLE for the caller's */
    uint64_t memory_bytes;
    int initialised;    /* the first record zeroes every image */
    VkSampler linear_clamp;
    Pipe pipe[FSR3_PASS_COUNT];
    VkDescriptorPool pool;
    VkDescriptorSet *sets; /* [slot][pass] */
    VkBuffer constants;
    VkDeviceMemory constants_memory;
    unsigned char *constants_map;
    VkDeviceSize constants_stride; /* one constant buffer, aligned */

    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
#define FSR3_FN(name) PFN_vk##name name
    FSR3_FN(CreateImage); FSR3_FN(DestroyImage); FSR3_FN(GetImageMemoryRequirements);
    FSR3_FN(AllocateMemory); FSR3_FN(FreeMemory); FSR3_FN(BindImageMemory); FSR3_FN(BindBufferMemory);
    FSR3_FN(MapMemory); FSR3_FN(UnmapMemory);
    FSR3_FN(CreateImageView); FSR3_FN(DestroyImageView); FSR3_FN(CreateBuffer); FSR3_FN(DestroyBuffer);
    FSR3_FN(GetBufferMemoryRequirements); FSR3_FN(CreateSampler); FSR3_FN(DestroySampler);
    FSR3_FN(CreateDescriptorSetLayout); FSR3_FN(DestroyDescriptorSetLayout);
    FSR3_FN(CreateDescriptorPool); FSR3_FN(DestroyDescriptorPool); FSR3_FN(AllocateDescriptorSets);
    FSR3_FN(UpdateDescriptorSets); FSR3_FN(CreatePipelineLayout); FSR3_FN(DestroyPipelineLayout);
    FSR3_FN(CreateShaderModule); FSR3_FN(DestroyShaderModule); FSR3_FN(CreateComputePipelines);
    FSR3_FN(DestroyPipeline); FSR3_FN(DeviceWaitIdle);
    FSR3_FN(CmdPipelineBarrier); FSR3_FN(CmdClearColorImage); FSR3_FN(CmdBindPipeline);
    FSR3_FN(CmdBindDescriptorSets); FSR3_FN(CmdDispatch);
#undef FSR3_FN
};

static void set_error(char *error, size_t cap, const char *text) {
    if (error && cap) snprintf(error, cap, "%s", text);
}

/* --- the device probe ------------------------------------------------------------------------- */

static int image_of(uint32_t id) {
    for (uint32_t i = 0; i < IMAGE_COUNT; i++)
        if (k_images[i].id == id) return (int)i;
    return -1;
}

const char *fsr3_pass_unavailable(PFN_vkGetInstanceProcAddr gipa, VkInstance instance, VkPhysicalDevice pd,
                                  VkFormat tc_format) {
    PFN_vkGetPhysicalDeviceFeatures features_fn =
        (PFN_vkGetPhysicalDeviceFeatures)gipa(instance, "vkGetPhysicalDeviceFeatures");
    PFN_vkGetPhysicalDeviceFormatProperties format_fn =
        (PFN_vkGetPhysicalDeviceFormatProperties)gipa(instance, "vkGetPhysicalDeviceFormatProperties");
    if (!features_fn || !format_fn) return "vkGetPhysicalDeviceFeatures";
    VkPhysicalDeviceFeatures f;
    features_fn(pd, &f);
    if (!f.shaderStorageImageReadWithoutFormat) return "shaderStorageImageReadWithoutFormat";
    if (!f.shaderStorageImageWriteWithoutFormat) return "shaderStorageImageWriteWithoutFormat";

    /* Every internal image is written as a storage image and read as a sampled one; those the
     * shaders sample with s_LinearClamp need linear filtering; atomics reach the R32_UINT ones. */
    static char reason[96];
    for (uint32_t i = 0; i < IMAGE_COUNT; i++) {
        VkFormatProperties fp;
        format_fn(pd, k_images[i].format, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                    VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        if (k_images[i].format == VK_FORMAT_R32_UINT) need |= VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT;
        for (uint32_t p = 0; p < FSR3_PASS_COUNT; p++)
            for (uint32_t b = 0; b < fsr3_pass_layout[p].count; b++) {
                const Fsr3Binding *bd = &fsr3_pass_layout[p].bindings[b];
                if (bd->kind == FSR3_BIND_SRV && bd->linear && image_of(bd->id) == (int)i)
                    need |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
            }
        if ((fp.optimalTilingFeatures & need) != need) {
            snprintf(reason, sizeof(reason), "format %d features 0x%x", (int)k_images[i].format,
                     (unsigned)(need & ~fp.optimalTilingFeatures));
            return reason;
        }
    }
    if (tc_format != VK_FORMAT_UNDEFINED) {
        VkFormatProperties fp;
        format_fn(pd, tc_format, &fp);
        const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
                                          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
        if ((fp.optimalTilingFeatures & need) != need) return "linear filtering of the transparency and composition format";
    }
    return NULL;
}

/* --- creation --------------------------------------------------------------------------------- */

static uint32_t memory_type(Fsr3Pass *p, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    p->GetPhysicalDeviceMemoryProperties(p->init.physical_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static VkImageView make_view(Fsr3Pass *p, VkImage image, VkFormat format, uint32_t base_mip, uint32_t mips) {
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

static int make_image(Fsr3Pass *p, uint32_t index) {
    const Fsr3HostDesc *d = &p->init.desc;
    uint32_t w = 1, h = 1;
    switch (k_images[index].size) {
    case SIZE_RENDER: w = d->max_render_width; h = d->max_render_height; break;
    case SIZE_HALF_RENDER: w = d->max_render_width / 2; h = d->max_render_height / 2; break;
    case SIZE_UPSCALE: w = d->max_upscale_width; h = d->max_upscale_height; break;
    default: break;
    }
    Img *im = &p->img[k_images[index].id];
    im->format = k_images[index].format;
    im->mips = k_images[index].full_chain ? chain_length(w, h) : 1;
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = im->format;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = im->mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
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
    if (k_images[index].id == R(SPD_MIPS)) {
        /* The SPD passes write levels 0 to 5; a smaller chain repeats its last level, which they
         * never reach (ffxSpdSetup's mip count is that of the render size). */
        for (uint32_t m = 0; m < SPD_UAV_MIPS; m++) {
            im->mip[m] = make_view(p, im->image, im->format, m < im->mips ? m : im->mips - 1, 1);
            if (!im->mip[m]) return 0;
        }
    }
    return 1;
}

static VkDescriptorType descriptor_type(Fsr3BindKind kind) {
    switch (kind) {
    case FSR3_BIND_CBV: return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    case FSR3_BIND_SAMPLER: return VK_DESCRIPTOR_TYPE_SAMPLER;
    case FSR3_BIND_SRV: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    default: return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
}

static int make_pipe(Fsr3Pass *p, uint32_t pass) {
    const Fsr3PassLayout *l = &fsr3_pass_layout[pass];
    Pipe *pp = &p->pipe[pass];
    VkDescriptorSetLayoutBinding b[24];
    if (l->count > sizeof(b) / sizeof(b[0])) return 0;
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
    VkComputePipelineCreateInfo ci;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    ci.stage.module = pp->module;
    ci.stage.pName = "CS";
    ci.layout = pp->layout;
    return p->CreateComputePipelines(p->dev, VK_NULL_HANDLE, 1, &ci, NULL, &pp->pipeline) == VK_SUCCESS;
}

static int make_constants(Fsr3Pass *p) {
    VkPhysicalDeviceProperties props;
    p->GetPhysicalDeviceProperties(p->init.physical_device, &props);
    const VkDeviceSize align = props.limits.minUniformBufferOffsetAlignment ? props.limits.minUniformBufferOffsetAlignment : 1;
    p->constants_stride = (256 + align - 1) / align * align;
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = p->constants_stride * 3 * p->init.frames_in_flight;
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

static int make_sets(Fsr3Pass *p) {
    const uint32_t slots = p->init.frames_in_flight;
    VkDescriptorPoolSize sizes[4] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 0 },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 0 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 0 },
    };
    for (uint32_t pass = 0; pass < FSR3_PASS_COUNT; pass++)
        for (uint32_t i = 0; i < fsr3_pass_layout[pass].count; i++)
            sizes[fsr3_pass_layout[pass].bindings[i].kind].descriptorCount += slots;
    VkDescriptorPoolCreateInfo pci;
    memset(&pci, 0, sizeof(pci));
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = slots * FSR3_PASS_COUNT;
    pci.poolSizeCount = 4;
    pci.pPoolSizes = sizes;
    if (p->CreateDescriptorPool(p->dev, &pci, NULL, &p->pool) != VK_SUCCESS) return 0;
    p->sets = calloc((size_t)slots * FSR3_PASS_COUNT, sizeof(*p->sets));
    if (!p->sets) return 0;
    for (uint32_t s = 0; s < slots; s++)
        for (uint32_t pass = 0; pass < FSR3_PASS_COUNT; pass++) {
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, p->pool, 1,
                                               &p->pipe[pass].dsl };
            if (p->AllocateDescriptorSets(p->dev, &ai, &p->sets[s * FSR3_PASS_COUNT + pass]) != VK_SUCCESS) return 0;
        }
    return 1;
}

Fsr3Pass *fsr3_pass_create(const Fsr3PassInit *init, char *error, size_t error_size) {
    const Fsr3HostDesc *d = &init->desc;
    if (!init->get_instance_proc_addr || !init->device || !init->frames_in_flight) {
        set_error(error, error_size, "FSR3: incomplete init");
        return NULL;
    }
    if (!(d->flags & FSR3_HOST_ENABLE_DEPTH_INVERTED)) {
        set_error(error, error_size, "FSR3: the shaders are built for inverted depth");
        return NULL;
    }
    if (d->max_render_width < 2 || d->max_render_height < 2 || d->max_upscale_width < 1 || d->max_upscale_height < 1) {
        set_error(error, error_size, "FSR3: the maximum sizes are too small");
        return NULL;
    }
    Fsr3Pass *p = calloc(1, sizeof(*p));
    if (!p) {
        set_error(error, error_size, "FSR3: out of memory");
        return NULL;
    }
    p->init = *init;
    p->dev = init->device;
    PFN_vkGetDeviceProcAddr dev_addr =
        (PFN_vkGetDeviceProcAddr)init->get_instance_proc_addr(init->instance, "vkGetDeviceProcAddr");
    p->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)init->get_instance_proc_addr(
        init->instance, "vkGetPhysicalDeviceMemoryProperties");
    p->GetPhysicalDeviceProperties =
        (PFN_vkGetPhysicalDeviceProperties)init->get_instance_proc_addr(init->instance, "vkGetPhysicalDeviceProperties");
    if (!dev_addr || !p->GetPhysicalDeviceMemoryProperties || !p->GetPhysicalDeviceProperties) {
        set_error(error, error_size, "FSR3: no Vulkan instance functions");
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
    DEV(CmdPipelineBarrier); DEV(CmdClearColorImage); DEV(CmdBindPipeline);
    DEV(CmdBindDescriptorSets); DEV(CmdDispatch);
#undef DEV
    if (!ok) {
        set_error(error, error_size, "FSR3: a Vulkan device function is missing");
        free(p);
        return NULL;
    }
    fsr3_host_init(&p->host, d);
    for (uint32_t i = 0; ok && i < IMAGE_COUNT; i++) ok = make_image(p, i);
    if (!ok) {
        set_error(error, error_size, "FSR3: cannot create an internal image");
        fsr3_pass_destroy(p);
        return NULL;
    }
    VkSamplerCreateInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = VK_FILTER_LINEAR;
    si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.minLod = 0.0f;
    si.maxLod = VK_LOD_CLAMP_NONE;
    si.borderColor = VK_BORDER_COLOR_INT_TRANSPARENT_BLACK;
    if (p->CreateSampler(p->dev, &si, NULL, &p->linear_clamp) != VK_SUCCESS) {
        set_error(error, error_size, "FSR3: cannot create the sampler");
        fsr3_pass_destroy(p);
        return NULL;
    }
    for (uint32_t pass = 0; ok && pass < FSR3_PASS_COUNT; pass++) ok = make_pipe(p, pass);
    if (!ok) {
        set_error(error, error_size, "FSR3: cannot create a pipeline");
        fsr3_pass_destroy(p);
        return NULL;
    }
    if (!make_constants(p) || !make_sets(p)) {
        set_error(error, error_size, "FSR3: cannot create the constants or descriptor sets");
        fsr3_pass_destroy(p);
        return NULL;
    }
    return p;
}

void fsr3_pass_destroy(Fsr3Pass *p) {
    if (!p) return;
    p->DeviceWaitIdle(p->dev);
    if (p->constants_map) p->UnmapMemory(p->dev, p->constants_memory);
    if (p->constants) p->DestroyBuffer(p->dev, p->constants, NULL);
    if (p->constants_memory) p->FreeMemory(p->dev, p->constants_memory, NULL);
    if (p->pool) p->DestroyDescriptorPool(p->dev, p->pool, NULL);
    free(p->sets);
    for (uint32_t pass = 0; pass < FSR3_PASS_COUNT; pass++) {
        Pipe *pp = &p->pipe[pass];
        if (pp->pipeline) p->DestroyPipeline(p->dev, pp->pipeline, NULL);
        if (pp->module) p->DestroyShaderModule(p->dev, pp->module, NULL);
        if (pp->layout) p->DestroyPipelineLayout(p->dev, pp->layout, NULL);
        if (pp->dsl) p->DestroyDescriptorSetLayout(p->dev, pp->dsl, NULL);
    }
    if (p->linear_clamp) p->DestroySampler(p->dev, p->linear_clamp, NULL);
    for (uint32_t r = 0; r < RESOURCES; r++) {
        Img *im = &p->img[r];
        for (uint32_t m = 0; m < SPD_UAV_MIPS; m++)
            if (im->mip[m]) p->DestroyImageView(p->dev, im->mip[m], NULL);
        if (im->view) p->DestroyImageView(p->dev, im->view, NULL);
        if (im->image) p->DestroyImage(p->dev, im->image, NULL);
        if (im->memory) p->FreeMemory(p->dev, im->memory, NULL);
    }
    free(p);
}

uint64_t fsr3_pass_memory_bytes(const Fsr3Pass *p) {
    return p ? p->memory_bytes : 0;
}

const Fsr3Host *fsr3_pass_host(const Fsr3Pass *p) {
    return &p->host;
}

/* --- recording -------------------------------------------------------------------------------- */

/* Every earlier compute or transfer write, before every later compute or transfer access. */
static void barrier_all(Fsr3Pass *p, VkCommandBuffer cmd) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT };
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    p->CmdPipelineBarrier(cmd, stages, stages, 0, 1, &mb, 0, NULL, 0, NULL);
}

/* The image's contents become undefined: a transition from UNDEFINED, after its earlier uses. */
static void transition_from_undefined(Fsr3Pass *p, VkCommandBuffer cmd, const Img *im) {
    VkImageMemoryBarrier ib;
    memset(&ib, 0, sizeof(ib));
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = im->image;
    ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ib.subresourceRange.levelCount = im->mips;
    ib.subresourceRange.layerCount = 1;
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    p->CmdPipelineBarrier(cmd, stages, stages, 0, 0, NULL, 0, NULL, 1, &ib);
}

static void clear(Fsr3Pass *p, VkCommandBuffer cmd, const Img *im, const float value[4]) {
    /* AMD's Vulkan backend writes the float colour into the union for every format, so an R32_UINT
     * image takes the float's bits, which is what its shaders compare. */
    VkClearColorValue v;
    memcpy(v.float32, value, sizeof(v.float32));
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, im->mips, 0, 1 };
    p->CmdClearColorImage(cmd, im->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
}

/* The view a resolved binding uses: an internal image, its SPD level, or one of the caller's. */
static VkImageView view_of(const Fsr3Pass *p, const Fsr3PassFrame *f, uint32_t resource, uint32_t mip, int storage) {
    switch (resource) {
    case R(INPUT_COLOR): return f->color;
    case R(INPUT_DEPTH): return f->depth;
    case R(INPUT_MOTION_VECTORS): return f->motion_vectors;
    case R(INPUT_EXPOSURE): return f->exposure;
    case R(INPUT_REACTIVE_MASK): return f->reactive;
    case R(INPUT_TRANSPARENCY_AND_COMPOSITION_MASK): return f->transparency_and_composition;
    case R(UPSCALED_OUTPUT): return f->output;
    default: break;
    }
    if (resource >= RESOURCES) return VK_NULL_HANDLE;
    const Img *im = &p->img[resource];
    if (storage && im->mips > 1) return mip < SPD_UAV_MIPS ? im->mip[mip] : VK_NULL_HANDLE;
    return mip == 0 ? im->view : VK_NULL_HANDLE;
}

static int is_input(uint32_t resource) {
    return resource == R(INPUT_COLOR) || resource == R(INPUT_DEPTH) || resource == R(INPUT_MOTION_VECTORS) ||
           resource == R(INPUT_EXPOSURE) || resource == R(INPUT_REACTIVE_MASK) ||
           resource == R(INPUT_TRANSPARENCY_AND_COMPOSITION_MASK);
}

/* Writes the slot's descriptor sets for the plan fsr3_host_dispatch just made. 0 when a binding
 * resolves to nothing, which the plan never does for a frame fsr3_pass_record accepted. */
static int write_sets(Fsr3Pass *p, const Fsr3PassFrame *f) {
    VkWriteDescriptorSet w[24];
    VkDescriptorImageInfo ii[24];
    VkDescriptorBufferInfo bi[24];
    const VkDeviceSize base = (VkDeviceSize)f->slot * 3 * p->constants_stride;
    for (uint32_t pass = 0; pass < FSR3_PASS_COUNT; pass++) {
        const Fsr3PassLayout *l = &fsr3_pass_layout[pass];
        for (uint32_t i = 0; i < l->count; i++) {
            const Fsr3Binding *b = &l->bindings[i];
            memset(&w[i], 0, sizeof(w[i]));
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstSet = p->sets[f->slot * FSR3_PASS_COUNT + pass];
            w[i].dstBinding = b->binding;
            w[i].descriptorCount = 1;
            w[i].descriptorType = descriptor_type(b->kind);
            if (b->kind == FSR3_BIND_CBV) {
                bi[i].buffer = p->constants;
                bi[i].offset = base + b->id * p->constants_stride;
                bi[i].range = p->constants_stride;
                w[i].pBufferInfo = &bi[i];
                continue;
            }
            memset(&ii[i], 0, sizeof(ii[i]));
            if (b->kind == FSR3_BIND_SAMPLER) {
                ii[i].sampler = p->linear_clamp;
            } else {
                uint32_t resource, mip;
                fsr3_host_resolve(&p->host, b, &resource, &mip);
                ii[i].imageView = view_of(p, f, resource, mip, b->kind == FSR3_BIND_UAV);
                ii[i].imageLayout = (b->kind == FSR3_BIND_SRV && is_input(resource)) ? f->input_layout
                                                                                     : VK_IMAGE_LAYOUT_GENERAL;
                if (!ii[i].imageView) return 0;
            }
            w[i].pImageInfo = &ii[i];
        }
        p->UpdateDescriptorSets(p->dev, l->count, w, 0, NULL);
    }
    return 1;
}

int fsr3_pass_record(Fsr3Pass *p, VkCommandBuffer cmd, const Fsr3PassFrame *f) {
    if (!p || !cmd || !f || f->slot >= p->init.frames_in_flight || !f->color || !f->depth || !f->motion_vectors ||
        !f->output)
        return -1;
    if (f->input_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && f->input_layout != VK_IMAGE_LAYOUT_GENERAL)
        return -1;
    Fsr3HostDispatch d = f->params;
    d.has_exposure = f->exposure != VK_NULL_HANDLE;
    d.has_reactive = f->reactive != VK_NULL_HANDLE;
    d.has_transparency_and_composition = f->transparency_and_composition != VK_NULL_HANDLE;
    /* A refused frame leaves the context as it was: no frame counted, no ping-pong flipped. */
    const Fsr3Host before = p->host;
    if (fsr3_host_dispatch(&p->host, &d) != 0 || !write_sets(p, f)) {
        p->host = before;
        return -1;
    }

    /* The constants: the slot's three buffers, in AMD's staging order. */
    unsigned char *c = p->constants_map + (size_t)f->slot * 3 * (size_t)p->constants_stride;
    memcpy(c + FFX_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_FSR3UPSCALER * p->constants_stride, &p->host.constants,
           sizeof(p->host.constants));
    memcpy(c + FFX_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_SPD * p->constants_stride, &p->host.spd, sizeof(p->host.spd));
    memcpy(c + FFX_FSR3UPSCALER_CONSTANTBUFFER_IDENTIFIER_RCAS * p->constants_stride, &p->host.rcas,
           sizeof(p->host.rcas));

    if (!p->initialised) {
        /* The first record defines every image: GENERAL, and zero, as a D3D12 committed resource starts.
         * AMD's own first-use clears follow in the plan. The 1x1 defaults hold AMD's initial values,
         * all zero: no reactivity and an exposure of (0, 0). */
        static const float zero[4] = { 0, 0, 0, 0 };
        for (uint32_t i = 0; i < IMAGE_COUNT; i++) transition_from_undefined(p, cmd, &p->img[k_images[i].id]);
        for (uint32_t i = 0; i < IMAGE_COUNT; i++) clear(p, cmd, &p->img[k_images[i].id], zero);
        p->initialised = 1;
    }
    const VkDescriptorSet *sets = &p->sets[f->slot * FSR3_PASS_COUNT];
    for (uint32_t i = 0; i < p->host.job_count; i++) {
        const Fsr3Job *j = &p->host.jobs[i];
        switch (j->kind) {
        case FSR3_JOB_CLEAR:
            barrier_all(p, cmd); /* AMD clears SPD_MIPS twice on a reset */
            clear(p, cmd, &p->img[j->resource], j->value);
            break;
        case FSR3_JOB_DISCARD:
            transition_from_undefined(p, cmd, &p->img[j->resource]);
            break;
        case FSR3_JOB_DISPATCH: {
            const Pipe *pp = &p->pipe[j->pass];
            barrier_all(p, cmd);
            p->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pp->pipeline);
            p->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pp->layout, 0, 1, &sets[j->pass], 0, NULL);
            p->CmdDispatch(cmd, j->groups_x, j->groups_y, 1);
            break;
        }
        }
    }
    return 0;
}
