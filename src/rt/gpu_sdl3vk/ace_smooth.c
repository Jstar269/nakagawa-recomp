/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_smooth.c - see ace_smooth.h.
 *
 * Every frame: the source is blitted into one of two private images with mips; a three-level
 * search over the mips fills three vector grids; the compose pass writes the in-between at the
 * frame's size. The grids are fixed, so the search costs the same at any render scale.
 *
 * The images are reused by the frames in flight, on one queue: each pass's barriers order it
 * after the previous present's use, and nothing here waits for the GPU except a size change.
 */
#include "ace_smooth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t k_match_spv[] =
#include "ace_match_comp.inc"
;
static const uint32_t k_compose_spv[] =
#include "ace_compose_comp.inc"
;

#define LEVELS 3
#define FIELD_FORMAT VK_FORMAT_R16G16B16A16_SFLOAT
#define FRAME_FORMAT VK_FORMAT_R8G8B8A8_UNORM
/* The search's sharpest level looks at a frame 480 samples wide, whatever the render scale; a
 * block is 4 of those, and the coarsest level reaches about 64 of them between two frames. */
#define SEARCH_W 480
static const struct { int cols, rows, down, window, range; } k_level[LEVELS] = {
    { 30, 17, 4, 8, 8 },
    { 60, 34, 2, 6, 2 },
    { 120, 68, 1, 6, 1 },
};
/* A cut: this share of the middle grid fits nothing or is torn from its neighbours. */
#define CUT_LEVEL 1
#define CUT_NUM 1
#define CUT_DEN 3

typedef struct {
    int32_t grid[2];
    float texel[2];
    float lod;
    int32_t window, range, first, count_bad;
} MatchPush;

typedef struct {
    int32_t size[2];
    uint32_t cut_blocks;
} ComposePush;
typedef char ace_push_fits[sizeof(ComposePush) <= sizeof(MatchPush) ? 1 : -1];

typedef struct {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkImageLayout layout;
} Img;

struct AceBetween {
    AceSmoothInit init;
    VkDevice dev;
    int w, h, mips;
    int cur;                 /* the newest frame */
    int have[2];
    Img frame[2];
    Img field[LEVELS];
    Img out;
    VkBuffer stats;
    VkDeviceMemory stats_memory;
    VkSampler linear, nearest;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool pool;
    /* [old frame index][pass]: pass 0..2 the levels, 3 compose. */
    VkDescriptorSet sets[2][LEVELS + 1];
    VkPipelineLayout layout;
    VkPipeline match, compose;
    VkShaderModule match_mod, compose_mod;

    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
#define ACE_FN(name) PFN_vk##name name
    ACE_FN(CreateImage); ACE_FN(DestroyImage); ACE_FN(GetImageMemoryRequirements);
    ACE_FN(AllocateMemory); ACE_FN(FreeMemory); ACE_FN(BindImageMemory); ACE_FN(BindBufferMemory);
    ACE_FN(CreateImageView); ACE_FN(DestroyImageView); ACE_FN(CreateBuffer); ACE_FN(DestroyBuffer);
    ACE_FN(GetBufferMemoryRequirements); ACE_FN(CreateSampler); ACE_FN(DestroySampler);
    ACE_FN(CreateDescriptorSetLayout); ACE_FN(DestroyDescriptorSetLayout);
    ACE_FN(CreateDescriptorPool); ACE_FN(DestroyDescriptorPool); ACE_FN(AllocateDescriptorSets);
    ACE_FN(UpdateDescriptorSets); ACE_FN(CreatePipelineLayout); ACE_FN(DestroyPipelineLayout);
    ACE_FN(CreateShaderModule); ACE_FN(DestroyShaderModule); ACE_FN(CreateComputePipelines);
    ACE_FN(DestroyPipeline); ACE_FN(DeviceWaitIdle);
    ACE_FN(CmdPipelineBarrier); ACE_FN(CmdBlitImage); ACE_FN(CmdBindPipeline);
    ACE_FN(CmdBindDescriptorSets); ACE_FN(CmdPushConstants); ACE_FN(CmdDispatch);
    ACE_FN(CmdFillBuffer);
#undef ACE_FN
};

static void set_error(char *error, size_t cap, const char *text) {
    if (error && cap) snprintf(error, cap, "%s", text);
}

static uint32_t memory_type(AceBetween *s, uint32_t bits) {
    VkPhysicalDeviceMemoryProperties mp;
    s->GetPhysicalDeviceMemoryProperties(s->init.physical_device, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    }
    return UINT32_MAX;
}

static int img_make(AceBetween *s, Img *im, VkFormat format, int w, int h, int mips,
                    VkImageUsageFlags usage) {
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = (uint32_t)mips;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (s->CreateImage(s->dev, &ii, NULL, &im->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    s->GetImageMemoryRequirements(s->dev, im->image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(s, mr.memoryTypeBits) };
    if (mai.memoryTypeIndex == UINT32_MAX ||
        s->AllocateMemory(s->dev, &mai, NULL, &im->memory) != VK_SUCCESS ||
        s->BindImageMemory(s->dev, im->image, im->memory, 0) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = im->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = (uint32_t)mips;
    vi.subresourceRange.layerCount = 1;
    im->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return s->CreateImageView(s->dev, &vi, NULL, &im->view) == VK_SUCCESS;
}

static void img_free(AceBetween *s, Img *im) {
    if (im->view) s->DestroyImageView(s->dev, im->view, NULL);
    if (im->image) s->DestroyImage(s->dev, im->image, NULL);
    if (im->memory) s->FreeMemory(s->dev, im->memory, NULL);
    memset(im, 0, sizeof(*im));
}

static void sized_free(AceBetween *s) {
    for (int i = 0; i < 2; i++) { img_free(s, &s->frame[i]); s->have[i] = 0; }
    for (int i = 0; i < LEVELS; i++) img_free(s, &s->field[i]);
    img_free(s, &s->out);
    s->w = s->h = 0;
}

static void write_sets(AceBetween *s) {
    VkDescriptorBufferInfo stats = { s->stats, 0, VK_WHOLE_SIZE };
    for (int old = 0; old < 2; old++) {
        for (int pass = 0; pass <= LEVELS; pass++) {
            int compose = pass == LEVELS;
            VkDescriptorImageInfo a = { s->linear, s->frame[old].view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo b = { s->linear, s->frame[!old].view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            /* The coarsest level has no level above: it binds its neighbour, unread. */
            const Img *read = &s->field[compose ? LEVELS - 1 : pass > 0 ? pass - 1 : 1];
            VkDescriptorImageInfo up = { s->nearest, read->view, VK_IMAGE_LAYOUT_GENERAL };
            VkDescriptorImageInfo store = { VK_NULL_HANDLE, compose ? s->out.view : s->field[pass].view,
                                            VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet w[5];
            memset(w, 0, sizeof(w));
            static const uint32_t binding[4] = { 0, 1, 2, 5 };
            for (int i = 0; i < 5; i++) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstSet = s->sets[old][pass];
                w[i].descriptorCount = 1;
                w[i].dstBinding = i < 4 ? binding[i] : (compose ? 4u : 3u);
                w[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                                    : i == 3 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                             : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            }
            w[0].pImageInfo = &a;
            w[1].pImageInfo = &b;
            w[2].pImageInfo = &up;
            w[3].pBufferInfo = &stats;
            w[4].pImageInfo = &store;
            s->UpdateDescriptorSets(s->dev, 5, w, 0, NULL);
        }
    }
}

static int sized_make(AceBetween *s, int w, int h) {
    s->DeviceWaitIdle(s->dev);
    sized_free(s);
    /* Mips down to the coarsest level's sample size, from the source's own size. */
    int mips = 1;
    while ((w >> (mips - 1)) > SEARCH_W / k_level[0].down && mips < 8) mips++;
    s->mips = mips;
    VkImageUsageFlags frame_use = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                  VK_IMAGE_USAGE_SAMPLED_BIT;
    for (int i = 0; i < 2; i++)
        if (!img_make(s, &s->frame[i], FRAME_FORMAT, w, h, mips, frame_use)) return 0;
    for (int i = 0; i < LEVELS; i++)
        if (!img_make(s, &s->field[i], FIELD_FORMAT, k_level[i].cols, k_level[i].rows, 1,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)) return 0;
    if (!img_make(s, &s->out, FRAME_FORMAT, w, h, 1,
                  VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) return 0;
    s->w = w;
    s->h = h;
    s->cur = 0;
    write_sets(s);
    return 1;
}

static void img_barrier(AceBetween *s, VkCommandBuffer cmd, Img *im, VkImageLayout to,
                        VkAccessFlags dst_access, VkPipelineStageFlags dst_stage,
                        int discard) {
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
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = 1;
    s->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
    im->layout = to;
}

static void level_barrier(AceBetween *s, VkCommandBuffer cmd, Img *im, int level,
                          VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = (uint32_t)level;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    s->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                          0, NULL, 0, NULL, 1, &b);
}

/* The source, then its mips, all left in TRANSFER_SRC_OPTIMAL. */
static void copy_frame(AceBetween *s, VkCommandBuffer cmd, VkImage src, int w, int h, Img *to) {
    img_barrier(s, cmd, to, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 1);
    VkImageBlit blit;
    memset(&blit, 0, sizeof(blit));
    blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    blit.srcSubresource.layerCount = 1;
    blit.dstSubresource = blit.srcSubresource;
    blit.srcOffsets[1].x = w; blit.srcOffsets[1].y = h; blit.srcOffsets[1].z = 1;
    blit.dstOffsets[1] = blit.srcOffsets[1];
    s->CmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
    for (int m = 1; m < s->mips; m++) {
        level_barrier(s, cmd, to, m - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        int sw = w >> (m - 1), sh = h >> (m - 1);
        memset(&blit, 0, sizeof(blit));
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.layerCount = 1;
        blit.srcSubresource.mipLevel = (uint32_t)(m - 1);
        blit.dstSubresource = blit.srcSubresource;
        blit.dstSubresource.mipLevel = (uint32_t)m;
        blit.srcOffsets[1].x = sw; blit.srcOffsets[1].y = sh; blit.srcOffsets[1].z = 1;
        blit.dstOffsets[1].x = sw > 1 ? sw / 2 : 1;
        blit.dstOffsets[1].y = sh > 1 ? sh / 2 : 1;
        blit.dstOffsets[1].z = 1;
        s->CmdBlitImage(cmd, to->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        to->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
    }
    level_barrier(s, cmd, to, s->mips - 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    to->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

static void field_written(AceBetween *s, VkCommandBuffer cmd, Img *im) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = im->image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    s->CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 0, NULL, 0, NULL, 1, &b);
}

static void stats_barrier(AceBetween *s, VkCommandBuffer cmd, VkAccessFlags from, VkAccessFlags to,
                          VkPipelineStageFlags from_stage) {
    VkBufferMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    b.srcAccessMask = from;
    b.dstAccessMask = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.buffer = s->stats;
    b.size = VK_WHOLE_SIZE;
    s->CmdPipelineBarrier(cmd, from_stage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 1, &b,
                          0, NULL);
}

VkImage ace_smooth_record(AceBetween *s, VkCommandBuffer cmd, VkImage src, int w, int h, int between) {
    if (!s || !cmd || !src || w <= 0 || h <= 0) return VK_NULL_HANDLE;
    if ((w != s->w || h != s->h) && !sized_make(s, w, h)) {
        sized_free(s);
        return VK_NULL_HANDLE;
    }
    int old = s->cur;
    int cur = !old;
    copy_frame(s, cmd, src, w, h, &s->frame[cur]);
    s->cur = cur;
    int have_old = s->have[old];
    s->have[cur] = 1;
    if (!between || !have_old) return VK_NULL_HANDLE;

    for (int i = 0; i < 2; i++)
        img_barrier(s, cmd, &s->frame[i], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0);
    for (int i = 0; i < LEVELS; i++)
        img_barrier(s, cmd, &s->field[i], VK_IMAGE_LAYOUT_GENERAL,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);
    img_barrier(s, cmd, &s->out, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 1);
    s->CmdFillBuffer(cmd, s->stats, 0, VK_WHOLE_SIZE, 0u);
    stats_barrier(s, cmd, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);

    s->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->match);
    int source_scale = w / SEARCH_W > 0 ? w / SEARCH_W : 1;
    int base_lod = 0;
    while ((1 << base_lod) < source_scale) base_lod++;
    for (int l = 0; l < LEVELS; l++) {
        MatchPush p;
        memset(&p, 0, sizeof(p));
        p.grid[0] = k_level[l].cols;
        p.grid[1] = k_level[l].rows;
        p.texel[0] = (float)k_level[l].down / (float)SEARCH_W;
        p.texel[1] = (float)k_level[l].down * ((float)w / (float)SEARCH_W) / (float)h;
        p.lod = (float)(base_lod + (k_level[l].down == 4 ? 2 : k_level[l].down == 2 ? 1 : 0));
        p.window = k_level[l].window;
        p.range = k_level[l].range;
        p.first = l == 0;
        p.count_bad = l == CUT_LEVEL;
        s->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 1,
                                 &s->sets[old][l], 0, NULL);
        s->CmdPushConstants(cmd, s->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
        s->CmdDispatch(cmd, ((uint32_t)k_level[l].cols + 7u) / 8u, ((uint32_t)k_level[l].rows + 7u) / 8u, 1);
        field_written(s, cmd, &s->field[l]);
    }
    stats_barrier(s, cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                  VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

    ComposePush cp;
    cp.size[0] = w;
    cp.size[1] = h;
    cp.cut_blocks = (uint32_t)(k_level[CUT_LEVEL].cols * k_level[CUT_LEVEL].rows) * CUT_NUM / CUT_DEN;
    s->CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->compose);
    s->CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, s->layout, 0, 1,
                             &s->sets[old][LEVELS], 0, NULL);
    s->CmdPushConstants(cmd, s->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(cp), &cp);
    s->CmdDispatch(cmd, ((uint32_t)w + 15u) / 16u, ((uint32_t)h + 15u) / 16u, 1);
    img_barrier(s, cmd, &s->out, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, 0);
    return s->out.image;
}

VkImage ace_smooth_held(AceBetween *s, VkCommandBuffer cmd) {
    if (!s || !cmd || !s->have[s->cur]) return VK_NULL_HANDLE;
    Img *im = &s->frame[s->cur];
    if (im->layout != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        img_barrier(s, cmd, im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0);
    return im->image;
}

void ace_smooth_size(const AceBetween *s, int *w, int *h) {
    if (w) *w = s ? s->w : 0;
    if (h) *h = s ? s->h : 0;
}

static VkShaderModule shader_make(AceBetween *s, const uint32_t *code, size_t bytes) {
    VkShaderModuleCreateInfo mi;
    memset(&mi, 0, sizeof(mi));
    mi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    mi.codeSize = bytes;
    mi.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    return s->CreateShaderModule(s->dev, &mi, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static VkPipeline pipeline_make(AceBetween *s, VkShaderModule m) {
    VkComputePipelineCreateInfo pi;
    memset(&pi, 0, sizeof(pi));
    pi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    pi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    pi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    pi.stage.module = m;
    pi.stage.pName = "main";
    pi.layout = s->layout;
    VkPipeline p = VK_NULL_HANDLE;
    return s->CreateComputePipelines(s->dev, VK_NULL_HANDLE, 1, &pi, NULL, &p) == VK_SUCCESS
               ? p : VK_NULL_HANDLE;
}

static int fixed_make(AceBetween *s) {
    VkSamplerCreateInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 16.0f;
    if (s->CreateSampler(s->dev, &si, NULL, &s->linear) != VK_SUCCESS) return 0;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (s->CreateSampler(s->dev, &si, NULL, &s->nearest) != VK_SUCCESS) return 0;

    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = 16;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (s->CreateBuffer(s->dev, &bi, NULL, &s->stats) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    s->GetBufferMemoryRequirements(s->dev, s->stats, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(s, mr.memoryTypeBits) };
    if (mai.memoryTypeIndex == UINT32_MAX ||
        s->AllocateMemory(s->dev, &mai, NULL, &s->stats_memory) != VK_SUCCESS ||
        s->BindBufferMemory(s->dev, s->stats, s->stats_memory, 0) != VK_SUCCESS) return 0;

    VkDescriptorSetLayoutBinding b[6];
    memset(b, 0, sizeof(b));
    for (uint32_t i = 0; i < 6; i++) {
        b[i].binding = i;
        b[i].descriptorCount = 1;
        b[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        b[i].descriptorType = i < 3 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER
                            : i == 5 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                     : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    }
    /* A set writes binding 3 or 4, the one its pipeline reads; the other is never used. */
    VkDescriptorSetLayoutCreateInfo dl;
    memset(&dl, 0, sizeof(dl));
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 6;
    dl.pBindings = b;
    if (s->CreateDescriptorSetLayout(s->dev, &dl, NULL, &s->dsl) != VK_SUCCESS) return 0;

    uint32_t sets = 2u * (LEVELS + 1u);
    VkDescriptorPoolSize sizes[3] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sets * 3u },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sets },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, sets },
    };
    VkDescriptorPoolCreateInfo dp;
    memset(&dp, 0, sizeof(dp));
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = sets;
    dp.poolSizeCount = 3;
    dp.pPoolSizes = sizes;
    if (s->CreateDescriptorPool(s->dev, &dp, NULL, &s->pool) != VK_SUCCESS) return 0;
    VkDescriptorSetLayout layouts[2 * (LEVELS + 1)];
    for (uint32_t i = 0; i < sets; i++) layouts[i] = s->dsl;
    VkDescriptorSetAllocateInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = s->pool;
    ai.descriptorSetCount = sets;
    ai.pSetLayouts = layouts;
    if (s->AllocateDescriptorSets(s->dev, &ai, &s->sets[0][0]) != VK_SUCCESS) return 0;

    VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, (uint32_t)sizeof(MatchPush) };
    VkPipelineLayoutCreateInfo pl;
    memset(&pl, 0, sizeof(pl));
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &s->dsl;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &range;
    if (s->CreatePipelineLayout(s->dev, &pl, NULL, &s->layout) != VK_SUCCESS) return 0;
    s->match_mod = shader_make(s, k_match_spv, sizeof(k_match_spv));
    s->compose_mod = shader_make(s, k_compose_spv, sizeof(k_compose_spv));
    if (!s->match_mod || !s->compose_mod) return 0;
    s->match = pipeline_make(s, s->match_mod);
    s->compose = pipeline_make(s, s->compose_mod);
    return s->match && s->compose;
}

AceBetween *ace_smooth_create(const AceSmoothInit *init, char *error, size_t error_size) {
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
    AceBetween *s = (AceBetween *)calloc(1, sizeof(*s));
    if (!s) {
        set_error(error, error_size, "out of memory");
        return NULL;
    }
    s->init = *init;
    s->dev = init->device;
    s->GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)
        init->get_instance_proc_addr(init->instance, "vkGetPhysicalDeviceMemoryProperties");
#define DEV(name) s->name = (PFN_vk##name)dev_addr(s->dev, "vk" #name)
    DEV(CreateImage); DEV(DestroyImage); DEV(GetImageMemoryRequirements); DEV(AllocateMemory);
    DEV(FreeMemory); DEV(BindImageMemory); DEV(BindBufferMemory); DEV(CreateImageView);
    DEV(DestroyImageView); DEV(CreateBuffer); DEV(DestroyBuffer); DEV(GetBufferMemoryRequirements);
    DEV(CreateSampler); DEV(DestroySampler); DEV(CreateDescriptorSetLayout);
    DEV(DestroyDescriptorSetLayout); DEV(CreateDescriptorPool); DEV(DestroyDescriptorPool);
    DEV(AllocateDescriptorSets); DEV(UpdateDescriptorSets); DEV(CreatePipelineLayout);
    DEV(DestroyPipelineLayout); DEV(CreateShaderModule); DEV(DestroyShaderModule);
    DEV(CreateComputePipelines); DEV(DestroyPipeline); DEV(DeviceWaitIdle);
    DEV(CmdPipelineBarrier); DEV(CmdBlitImage); DEV(CmdBindPipeline); DEV(CmdBindDescriptorSets);
    DEV(CmdPushConstants); DEV(CmdDispatch); DEV(CmdFillBuffer);
#undef DEV
    if (!fixed_make(s)) {
        set_error(error, error_size, "a Vulkan object could not be made");
        ace_smooth_destroy(s);
        return NULL;
    }
    return s;
}

void ace_smooth_destroy(AceBetween *s) {
    if (!s) return;
    s->DeviceWaitIdle(s->dev);
    sized_free(s);
    if (s->match) s->DestroyPipeline(s->dev, s->match, NULL);
    if (s->compose) s->DestroyPipeline(s->dev, s->compose, NULL);
    if (s->match_mod) s->DestroyShaderModule(s->dev, s->match_mod, NULL);
    if (s->compose_mod) s->DestroyShaderModule(s->dev, s->compose_mod, NULL);
    if (s->layout) s->DestroyPipelineLayout(s->dev, s->layout, NULL);
    if (s->pool) s->DestroyDescriptorPool(s->dev, s->pool, NULL);
    if (s->dsl) s->DestroyDescriptorSetLayout(s->dev, s->dsl, NULL);
    if (s->stats) s->DestroyBuffer(s->dev, s->stats, NULL);
    if (s->stats_memory) s->FreeMemory(s->dev, s->stats_memory, NULL);
    if (s->linear) s->DestroySampler(s->dev, s->linear, NULL);
    if (s->nearest) s->DestroySampler(s->dev, s->nearest, NULL);
    free(s);
}
