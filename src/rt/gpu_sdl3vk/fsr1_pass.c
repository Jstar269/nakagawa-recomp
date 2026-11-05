/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr1_pass.c - see fsr1_pass.h.
 *
 * Every present: the shown image is blitted 1:1 into a private RGBA8 image (which also turns a
 * BGRA source into the layout the shaders read), EASU writes the upscale into a second image at
 * the output size, and RCAS sharpens that into a third. The images are reused by the frames in
 * flight, on one queue: each pass's barriers order it after the previous present's use, and
 * nothing here waits for the GPU except a size change.
 */
#include "fsr1_pass.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* AMD's CPU-side constant setup (FsrEasuCon, FsrRcasCon), from the byte-identical headers. They
 * define helpers this file does not call, and return const-qualified values. */
#define A_CPU 1
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../../third_party/fidelityfx_fsr1/ffx_a.h"
#include "../../../third_party/fidelityfx_fsr1/ffx_fsr1.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

static const uint32_t k_easu_spv[] =
#include "fsr1_easu_comp.inc"
;
static const uint32_t k_rcas_spv[] =
#include "fsr1_rcas_comp.inc"
;

#define FRAME_FORMAT VK_FORMAT_R8G8B8A8_UNORM

typedef struct {
    uint32_t con[4][4];
    uint32_t size[4];
} EasuPush;

typedef struct {
    uint32_t con[4];
    uint32_t size[4];
} RcasPush;

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageLayout layout;
} Img;

struct Fsr1Pass {
    Fsr1Init init;
    VkDevice dev;
    int w, h, ow, oh;
    Img in, mid, out;
    VkSampler linear, nearest;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    VkDescriptorSet easu_set, rcas_set;
    VkPipelineLayout layout;
    VkPipeline easu, rcas;
    VkShaderModule easu_mod, rcas_mod;

    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
#define FSR_FN(name) PFN_vk##name name
    FSR_FN(CreateImage); FSR_FN(DestroyImage); FSR_FN(GetImageMemoryRequirements);
    FSR_FN(AllocateMemory); FSR_FN(FreeMemory); FSR_FN(BindImageMemory);
    FSR_FN(CreateImageView); FSR_FN(DestroyImageView); FSR_FN(CreateSampler); FSR_FN(DestroySampler);
    FSR_FN(CreateDescriptorSetLayout); FSR_FN(DestroyDescriptorSetLayout);
    FSR_FN(CreateDescriptorPool); FSR_FN(DestroyDescriptorPool); FSR_FN(AllocateDescriptorSets);
    FSR_FN(UpdateDescriptorSets); FSR_FN(CreatePipelineLayout); FSR_FN(DestroyPipelineLayout);
    FSR_FN(CreateShaderModule); FSR_FN(DestroyShaderModule); FSR_FN(CreateComputePipelines);
    FSR_FN(DestroyPipeline); FSR_FN(DeviceWaitIdle);
    FSR_FN(CmdPipelineBarrier); FSR_FN(CmdBlitImage); FSR_FN(CmdBindPipeline);
    FSR_FN(CmdBindDescriptorSets); FSR_FN(CmdPushConstants); FSR_FN(CmdDispatch);
#undef FSR_FN
};

int fsr1_parse_mode(const char *value) {
    if (!value || !value[0] || strcmp(value, "off") == 0) return 0;
    if (strcmp(value, "fsr1") == 0) return 1;
    return -1;
}

static void set_error(char *error, size_t cap, const char *text) {
    if (error && cap) snprintf(error, cap, "%s", text);
}

static uint32_t memory_type(Fsr1Pass *p, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp;
    p->GetPhysicalDeviceMemoryProperties(p->init.physical_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    }
    return UINT32_MAX;
}

static int img_make(Fsr1Pass *p, Img *im, int w, int h, VkImageUsageFlags usage) {
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = FRAME_FORMAT;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (p->CreateImage(p->dev, &ii, NULL, &im->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    p->GetImageMemoryRequirements(p->dev, im->image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(p, mr.memoryTypeBits) };
    if (mai.memoryTypeIndex == UINT32_MAX ||
        p->AllocateMemory(p->dev, &mai, NULL, &im->memory) != VK_SUCCESS ||
        p->BindImageMemory(p->dev, im->image, im->memory, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = im->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = FRAME_FORMAT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return p->CreateImageView(p->dev, &vi, NULL, &im->view) == VK_SUCCESS;
}

static void img_free(Fsr1Pass *p, Img *im) {
    if (im->view) p->DestroyImageView(p->dev, im->view, NULL);
    if (im->image) p->DestroyImage(p->dev, im->image, NULL);
    if (im->memory) p->FreeMemory(p->dev, im->memory, NULL);
    memset(im, 0, sizeof(*im));
}

static void sized_free(Fsr1Pass *p) {
    img_free(p, &p->in);
    img_free(p, &p->mid);
    img_free(p, &p->out);
    p->w = p->h = p->ow = p->oh = 0;
}

static void set_write(Fsr1Pass *p, VkDescriptorSet set, VkSampler sampler, const Img *read,
                      const Img *store) {
    VkDescriptorImageInfo a = { sampler, read->view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo b = { VK_NULL_HANDLE, store->view, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[2];
    memset(w, 0, sizeof(w));
    for (int i = 0; i < 2; i++) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = set;
        w[i].dstBinding = (uint32_t)i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                     : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    w[0].pImageInfo = &a;
    w[1].pImageInfo = &b;
    p->UpdateDescriptorSets(p->dev, 2, w, 0, NULL);
}

static int sized_make(Fsr1Pass *p, int w, int h, int ow, int oh) {
    p->DeviceWaitIdle(p->dev);
    sized_free(p);
    if (!img_make(p, &p->in, w, h, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT) ||
        !img_make(p, &p->mid, ow, oh, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT) ||
        !img_make(p, &p->out, ow, oh, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return 0;
    set_write(p, p->easu_set, p->linear, &p->in, &p->mid);
    set_write(p, p->rcas_set, p->nearest, &p->mid, &p->out);
    p->w = w;
    p->h = h;
    p->ow = ow;
    p->oh = oh;
    return 1;
}

static void img_barrier(Fsr1Pass *p, VkCommandBuffer cmd, Img *im, VkImageLayout to,
                        VkAccessFlags dst_access, VkPipelineStageFlags dst_stage, int discard) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = dst_access;
    b.oldLayout = discard ? VK_IMAGE_LAYOUT_UNDEFINED : im->layout;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    p->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
    im->layout = to;
}

static void run_pass(Fsr1Pass *p, VkCommandBuffer cmd, VkPipeline pipe, VkDescriptorSet set,
                     const void *push, uint32_t push_bytes) {
    p->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    p->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, 1, &set, 0, NULL);
    p->CmdPushConstants(cmd, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
    p->CmdDispatch(cmd, ((uint32_t)p->ow + 7u) / 8u, ((uint32_t)p->oh + 7u) / 8u, 1);
}

VkImage fsr1_record(Fsr1Pass *p, VkCommandBuffer cmd, VkImage src, int w, int h, int out_w,
                    int out_h, float sharpness) {
    if (!p || !cmd || !src || w <= 0 || h <= 0 || out_w <= 0 || out_h <= 0) return VK_NULL_HANDLE;
    if ((w != p->w || h != p->h || out_w != p->ow || out_h != p->oh) &&
        !sized_make(p, w, h, out_w, out_h)) {
        sized_free(p);
        return VK_NULL_HANDLE;
    }

    /* The shown image, 1:1, into the private input. */
    img_barrier(p, cmd, &p->in, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
    VkImageBlit blit;
    memset(&blit, 0, sizeof(blit));
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1].x = w; blit.srcOffsets[1].y = h; blit.srcOffsets[1].z = 1;
    blit.dstOffsets[1] = blit.srcOffsets[1];
    p->CmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->in.image,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    img_barrier(p, cmd, &p->in, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0);

    /* EASU: input -> mid at the output size. */
    img_barrier(p, cmd, &p->mid, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);
    EasuPush ep;
    memset(&ep, 0, sizeof(ep));
    FsrEasuCon(ep.con[0], ep.con[1], ep.con[2], ep.con[3], (AF1)w, (AF1)h, (AF1)w, (AF1)h,
               (AF1)out_w, (AF1)out_h);
    ep.size[0] = (uint32_t)out_w;
    ep.size[1] = (uint32_t)out_h;
    run_pass(p, cmd, p->easu, p->easu_set, &ep, (uint32_t)sizeof(ep));

    if (sharpness <= FSR1_SHARPNESS_OFF) {
        img_barrier(p, cmd, &p->mid, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0);
        return p->mid.image;
    }

    /* RCAS: mid -> out. */
    img_barrier(p, cmd, &p->mid, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0);
    img_barrier(p, cmd, &p->out, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);
    RcasPush rp;
    memset(&rp, 0, sizeof(rp));
    FsrRcasCon(rp.con, (AF1)sharpness);
    rp.size[0] = (uint32_t)out_w;
    rp.size[1] = (uint32_t)out_h;
    run_pass(p, cmd, p->rcas, p->rcas_set, &rp, (uint32_t)sizeof(rp));
    img_barrier(p, cmd, &p->out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0);
    return p->out.image;
}

static VkShaderModule shader_make(Fsr1Pass *p, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo mi;
    memset(&mi, 0, sizeof(mi));
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    return p->CreateShaderModule(p->dev, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static VkPipeline pipeline_make(Fsr1Pass *p, VkShaderModule m) {
    VkComputePipelineCreateInfo pi;
    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = m;
    pi.stage.pName = "main";
    pi.layout = p->layout;
    VkPipeline pipe = VK_NULL_HANDLE;
    return p->CreateComputePipelines(p->dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipe) == VK_SUCCESS
               ? pipe : VK_NULL_HANDLE;
}

static int fixed_make(Fsr1Pass *p) {
    VkSamplerCreateInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (p->CreateSampler(p->dev, &si, NULL, &p->linear) != VK_SUCCESS) return 0;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (p->CreateSampler(p->dev, &si, NULL, &p->nearest) != VK_SUCCESS) return 0;

    VkDescriptorSetLayoutBinding b[2];
    memset(b, 0, sizeof(b));
    for (uint32_t i = 0; i < 2; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                     : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    VkDescriptorSetLayoutCreateInfo dl;
    memset(&dl, 0, sizeof(dl));
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 2;
    dl.pBindings = b;
    if (p->CreateDescriptorSetLayout(p->dev, &dl, NULL, &p->dsl) != VK_SUCCESS) return 0;

    VkDescriptorPoolSize sizes[2] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 },
    };
    VkDescriptorPoolCreateInfo dp;
    memset(&dp, 0, sizeof(dp));
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 2;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = sizes;
    if (p->CreateDescriptorPool(p->dev, &dp, NULL, &p->pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout layouts[2] = { p->dsl, p->dsl };
    VkDescriptorSet sets[2];
    VkDescriptorSetAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = p->pool;
    ai.descriptorSetCount = 2;
    ai.pSetLayouts = layouts;
    if (p->AllocateDescriptorSets(p->dev, &ai, sets) != VK_SUCCESS) return 0;
    p->easu_set = sets[0];
    p->rcas_set = sets[1];

    VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)sizeof(EasuPush) };
    VkPipelineLayoutCreateInfo pl;
    memset(&pl, 0, sizeof(pl));
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &p->dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (p->CreatePipelineLayout(p->dev, &pl, NULL, &p->layout) != VK_SUCCESS) return 0;
    if (p->init.easu_spirv && p->init.easu_spirv_bytes)
        p->easu_mod = shader_make(p, p->init.easu_spirv, p->init.easu_spirv_bytes);
    else
        p->easu_mod = shader_make(p, k_easu_spv, sizeof(k_easu_spv));
    p->rcas_mod = shader_make(p, k_rcas_spv, sizeof(k_rcas_spv));
    if (!p->easu_mod || !p->rcas_mod) return 0;
    p->easu = pipeline_make(p, p->easu_mod);
    p->rcas = pipeline_make(p, p->rcas_mod);
    return p->easu && p->rcas;
}

Fsr1Pass *fsr1_create(const Fsr1Init *init, char *error, size_t error_size) {
    if (!init || !init->get_instance_proc_addr || !init->device) {
        set_error(error, error_size, "no Vulkan device");
        return NULL;
    }
    PFN_vkGetPhysicalDeviceQueueFamilyProperties families =
        (PFN_vkGetPhysicalDeviceQueueFamilyProperties)init->get_instance_proc_addr(
            init->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    PFN_vkGetDeviceProcAddr dev_addr =
        (PFN_vkGetDeviceProcAddr)init->get_instance_proc_addr(init->instance, "vkGetDeviceProcAddr");
    if (!families || !dev_addr) {
        set_error(error, error_size, "the Vulkan loader is incomplete");
        return NULL;
    }
    VkQueueFamilyProperties qf[32];
    uint32_t nq = 32;
    families(init->physical_device, &nq, qf);
    if (init->queue_family >= nq || !(qf[init->queue_family].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
        set_error(error, error_size, "this GPU's queue has no compute");
        return NULL;
    }
    Fsr1Pass *p = (Fsr1Pass *)calloc(1, sizeof(*p));
    if (!p) {
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    p->init = *init;
    p->dev = init->device;
    p->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
        init->get_instance_proc_addr(init->instance, "vkGetPhysicalDeviceMemoryProperties");
#define DEV(name) p->name = (PFN_vk##name)dev_addr(p->dev, "vk" #name)
    DEV(CreateImage); DEV(DestroyImage); DEV(GetImageMemoryRequirements); DEV(AllocateMemory);
    DEV(FreeMemory); DEV(BindImageMemory); DEV(CreateImageView); DEV(DestroyImageView);
    DEV(CreateSampler); DEV(DestroySampler); DEV(CreateDescriptorSetLayout);
    DEV(DestroyDescriptorSetLayout); DEV(CreateDescriptorPool); DEV(DestroyDescriptorPool);
    DEV(AllocateDescriptorSets); DEV(UpdateDescriptorSets); DEV(CreatePipelineLayout);
    DEV(DestroyPipelineLayout); DEV(CreateShaderModule); DEV(DestroyShaderModule);
    DEV(CreateComputePipelines); DEV(DestroyPipeline); DEV(DeviceWaitIdle);
    DEV(CmdPipelineBarrier); DEV(CmdBlitImage); DEV(CmdBindPipeline); DEV(CmdBindDescriptorSets);
    DEV(CmdPushConstants); DEV(CmdDispatch);
#undef DEV
    if (!fixed_make(p)) {
        set_error(error, error_size, "a Vulkan object could not be made");
        fsr1_destroy(p);
        return NULL;
    }
    return p;
}

void fsr1_destroy(Fsr1Pass *p) {
    if (!p) return;
    p->DeviceWaitIdle(p->dev);
    sized_free(p);
    if (p->easu) p->DestroyPipeline(p->dev, p->easu, NULL);
    if (p->rcas) p->DestroyPipeline(p->dev, p->rcas, NULL);
    if (p->easu_mod) p->DestroyShaderModule(p->dev, p->easu_mod, NULL);
    if (p->rcas_mod) p->DestroyShaderModule(p->dev, p->rcas_mod, NULL);
    if (p->layout) p->DestroyPipelineLayout(p->dev, p->layout, NULL);
    if (p->pool) p->DestroyDescriptorPool(p->dev, p->pool, NULL);
    if (p->dsl) p->DestroyDescriptorSetLayout(p->dev, p->dsl, NULL);
    if (p->linear) p->DestroySampler(p->dev, p->linear, NULL);
    if (p->nearest) p->DestroySampler(p->dev, p->nearest, NULL);
    free(p);
}
