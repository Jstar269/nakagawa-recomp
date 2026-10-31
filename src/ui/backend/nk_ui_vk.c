/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Vulkan backend for nk_ui (lane C3c).

   One draw list, drawn into a render pass the host owns, in two steps
   (nk_ui_vk.h): prepare, outside the render pass, builds the frame's vertex and
   index buffers and records the texture uploads it needs; render, inside it,
   replays the draws, each with a scissor taken from its command's clip.

   Rectangles, borders and rounded-corner fans are the list's own triangles,
   sampled from a 1 x 1 white texture. Text is shaped and rasterised here through
   the host's NkUiTextBackend into a per-scale glyph atlas: pages of 1024 x 1024
   R8 coverage, viewed with an (r, r, r, r) swizzle so a texel reads as
   premultiplied white, sampled with nearest filtering. Image commands
   (NK_UI_CUSTOM_IMAGE) draw an RGBA8 texture uploaded once per NkUiImage.serial
   (and per drawn size when the image is shrunk, which is area-averaged on the
   CPU first) and kept in a small least recently used cache. Waves
   (NK_UI_CUSTOM_WAVES) are rendered by the CPU reference at a third of their
   size and scaled up with a linear filter. All of this follows the SDL_Renderer
   backend (nk_ui_sdlr.c), so the two draw alike.

   Blending is premultiplied alpha (ONE, ONE_MINUS_SRC_ALPHA for colour and
   alpha). A vertex's rgba_pm is written to the GPU vertex as R8G8B8A8 bytes.

   Resources a frame replaces (atlas pages, evicted or one-off images, a resized
   waves texture) may still be read by frames in flight, so they wait on the
   frame slot's list and are freed when that slot is prepared again.

   Compiled only with NK_UI_WITH_VULKAN; otherwise every entry is a stub. */

#if defined(NK_UI_WITH_VULKAN) && !defined(VK_NO_PROTOTYPES)
/* Every Vulkan call goes through a pointer loaded from the host's
   vkGetInstanceProcAddr; the loader's own exports are never linked. */
#define VK_NO_PROTOTYPES 1
#endif

#include "nk_ui_vk.h"

#include <stdlib.h>
#include <string.h>

#if !defined(NK_UI_WITH_VULKAN)

bool nk_ui_vk_available(void) {
    return false;
}

NkUiVk *nk_ui_vk_create(const NkUiVkInit *init) {
    (void)init;
    return NULL;
}

void nk_ui_vk_destroy(NkUiVk *vk) {
    (void)vk;
}

void nk_ui_vk_set_scale(NkUiVk *vk, float density) {
    (void)vk;
    (void)density;
}

bool nk_ui_vk_prepare(NkUiVk *vk, const NkUiDrawList *list,
                      struct VkCommandBuffer_T *command_buffer, uint32_t frame_slot) {
    (void)vk;
    (void)list;
    (void)command_buffer;
    (void)frame_slot;
    return false;
}

bool nk_ui_vk_render(NkUiVk *vk, const NkUiDrawList *list,
                     struct VkCommandBuffer_T *command_buffer,
                     uint32_t width, uint32_t height) {
    (void)vk;
    (void)list;
    (void)command_buffer;
    (void)width;
    (void)height;
    return false;
}

void nk_ui_vk_get_stats(const NkUiVk *vk, NkUiVkStats *out) {
    (void)vk;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

#else /* NK_UI_WITH_VULKAN */

#include "nk_ui_image.h"
#include "nk_ui_waves.h"

#include <limits.h>
#include <math.h>
#include <stddef.h>

#define NK_UI_VK_PAGE_SIZE 1024
#define NK_UI_VK_MAX_PAGES 8
#define NK_UI_VK_PAD 1
#define NK_UI_VK_MAX_GLYPHS_PER_RUN 8192
/* The waves are soft, so they render on the CPU at a third of the element's
   size (at most 480 px wide) and are scaled up with a linear filter. */
#define NK_UI_VK_WAVES_DIVISOR 3
#define NK_UI_VK_WAVES_MAX_W 480
/* Waves commands one list may hold: each needs its own texture for the frame,
   because every upload is recorded before any draw. The launcher draws one. */
#define NK_UI_VK_WAVES_SLOTS 4
/* Image textures kept alive: a library page of disc art, the title's picture
   and every prompt glyph set fit with room to spare. */
#define NK_UI_VK_IMAGE_CACHE 64
/* Larger images are left out (nk_ui decoders cap them far lower). */
#define NK_UI_VK_IMAGE_MAX_SIDE 8192
#define NK_UI_VK_POOL_SETS 64u
#define NK_UI_VK_UPLOAD_ALIGN ((size_t)16)
#define NK_UI_VK_MIN_BUFFER ((VkDeviceSize)65536)

static const uint32_t k_nk_ui_vert_spv[] =
#include "../shaders/ui_vert.inc"
;
static const uint32_t k_nk_ui_frag_spv[] =
#include "../shaders/ui_frag.inc"
;

/* ---- function table ---- */

#define NK_UI_VK_DEVICE_FUNCS(X) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
    X(AllocateMemory) X(FreeMemory) X(MapMemory) X(UnmapMemory) X(FlushMappedMemoryRanges) \
    X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
    X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) \
    X(CreateDescriptorSetLayout) X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) \
    X(DestroyDescriptorPool) X(AllocateDescriptorSets) X(FreeDescriptorSets) \
    X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) \
    X(CreateShaderModule) X(DestroyShaderModule) X(CreateGraphicsPipelines) \
    X(DestroyPipeline) X(QueueWaitIdle) X(CmdPipelineBarrier) X(CmdCopyBufferToImage) \
    X(CmdClearColorImage) X(CmdBindPipeline) X(CmdBindDescriptorSets) \
    X(CmdBindVertexBuffers) X(CmdBindIndexBuffer) X(CmdSetViewport) X(CmdSetScissor) \
    X(CmdPushConstants) X(CmdDrawIndexed)

typedef struct {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties GetPhysicalDeviceQueueFamilyProperties;
#define NK_UI_VK_FIELD(name) PFN_vk##name name;
    NK_UI_VK_DEVICE_FUNCS(NK_UI_VK_FIELD)
#undef NK_UI_VK_FIELD
} NkUiVkFns;

/* ---- types ---- */

/* The GPU vertex: NkUiVertex with its colour as R8G8B8A8 bytes. */
typedef struct {
    float x;
    float y;
    float u;
    float v;
    uint8_t rgba[4];
} NkUiVkVertex;

typedef struct NkUiVkTexture NkUiVkTexture;
struct NkUiVkTexture {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkDescriptorSet set;
    VkDescriptorPool pool;      /* the pool `set` came from */
    int width;
    int height;
    /* An upload into it has been recorded, so its layout is
       SHADER_READ_ONLY_OPTIMAL at every later command buffer boundary. */
    bool initialized;
    uint64_t touch_serial;      /* the prepare that queued an upload into it */
    size_t touch_index;         /* its NkUiVkTouched entry in that prepare */
    NkUiVkTexture *next;        /* in a frame slot's garbage list */
};

typedef struct {
    VkBuffer buffer;
    VkDeviceMemory memory;
    VkDeviceSize size;
    void *mapped;
    bool coherent;
} NkUiVkBuffer;

typedef struct {
    NkUiVkBuffer vertices;
    NkUiVkBuffer indices;
    NkUiVkBuffer staging;
    NkUiVkTexture *garbage;     /* retired while this slot was the newest */
} NkUiVkFrame;

typedef struct {
    int font;
    int px;
    uint32_t glyph;
    int page;       /* -1: cached as empty or too large, nothing to draw */
    int x;
    int y;
    int w;
    int h;
    int bearing_x;
    int bearing_y;
} NkUiVkEntry;

typedef struct {
    NkUiVkTexture *texture;
    int cursor_x;
    int cursor_y;
    int row_height;
} NkUiVkPage;

typedef struct {
    uint64_t serial;      /* NkUiImage.serial */
    int width;            /* the texture's size: the image's own, or a shrunk copy's */
    int height;
    uint64_t last_used;   /* prepare serial when last drawn */
    NkUiVkTexture *texture;
} NkUiVkImageEntry;

typedef struct {
    NkUiVkTexture *texture;
    uint8_t *pixels;
    int w;
    int h;
    bool valid;
    NkUiWavesCommand last;
} NkUiVkWaves;

/* An image this prepare uploads into, and how it is laid out before. */
typedef struct {
    VkImage image;
    bool was_initialized;
    bool clear;
} NkUiVkTouched;

typedef struct {
    size_t touched;
    VkDeviceSize offset;
    int x;
    int y;
    int w;
    int h;
} NkUiVkCopy;

/* One vkCmdDrawIndexed of the prepared frame. */
typedef struct {
    uint32_t first;
    uint32_t count;
    VkDescriptorSet set;
    int clip[4];
} NkUiVkDraw;

struct NkUiVk {
    NkUiVkFns fn;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    const VkAllocationCallbacks *allocator;
    VkPhysicalDeviceMemoryProperties memory;
    uint32_t frames_in_flight;
    NkUiTextBackend text;
    float density;
    int page_size;
    int image_max_side;

    VkSampler nearest;
    VkSampler linear;
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;
    VkDescriptorPool *pools;
    size_t pool_count;
    size_t pool_capacity;

    NkUiVkTexture *white;
    NkUiVkPage pages[NK_UI_VK_MAX_PAGES];
    int page_count;
    NkUiVkEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
    int32_t *table;
    size_t table_capacity;
    NkUiGlyph *glyphs;
    size_t glyph_capacity;
    NkUiVkImageEntry images[NK_UI_VK_IMAGE_CACHE];
    NkUiVkWaves waves[NK_UI_VK_WAVES_SLOTS];
    NkUiVkFrame frames[NK_UI_VK_MAX_FRAMES];
    /* The slot whose garbage takes what is replaced now: the slot being
       prepared, else the newest prepared; -1 before the first prepare, when
       nothing can be in flight and a replaced resource is freed at once. */
    int retire_slot;
    uint64_t serial;            /* prepare calls, ever */

    /* The frame being prepared. */
    const NkUiDrawList *list;
    NkUiVkVertex *extra_vertices;   /* glyph, image and waves quads */
    uint32_t extra_vertex_count;
    size_t extra_vertex_capacity;
    uint32_t *extra_indices;
    uint32_t extra_index_count;
    size_t extra_index_capacity;
    NkUiVkDraw *draws;
    size_t draw_count;
    size_t draw_capacity;
    uint8_t *upload;
    size_t upload_size;
    size_t upload_capacity;
    NkUiVkTouched *touched;
    size_t touched_count;
    size_t touched_capacity;
    VkImageMemoryBarrier *barriers;  /* one per touched image, for recording */
    size_t barrier_capacity;
    NkUiVkCopy *copies;
    size_t copy_count;
    size_t copy_capacity;
    VkBufferImageCopy *regions;      /* one per copy, for recording */
    size_t region_capacity;
    bool atlas_pending;             /* this prepare queued an atlas upload */
    uint32_t waves_used;

    /* The frame render draws. */
    bool prepared;
    const NkUiDrawList *prepared_list;
    const NkUiVertex *prepared_v;
    const uint32_t *prepared_idx;
    const NkUiDrawCmd *prepared_cmd;
    uint32_t prepared_nv;
    uint32_t prepared_ni;
    uint32_t prepared_ncmd;
    uint32_t prepared_slot;

    NkUiVkStats stats;
    NkUiVkStats delta;  /* this prepare's counts, added when it succeeds */
};

bool nk_ui_vk_available(void) {
    return true;
}

/* ---- small helpers ---- */

static bool nk_ui_vk_grow(void **array, size_t *capacity, size_t needed, size_t element) {
    size_t grown;
    void *memory;
    if (needed <= *capacity) {
        return true;
    }
    grown = *capacity == 0 ? 16u : *capacity;
    while (grown < needed) {
        if (grown > SIZE_MAX / 2u) {
            return false;
        }
        grown *= 2u;
    }
    if (grown > SIZE_MAX / element) {
        return false;
    }
    memory = realloc(*array, grown * element);
    if (memory == NULL) {
        return false;
    }
    *array = memory;
    *capacity = grown;
    return true;
}

static void nk_ui_vk_color(uint8_t out[4], uint32_t rgba_pm) {
    out[0] = (uint8_t)(rgba_pm >> 24);
    out[1] = (uint8_t)(rgba_pm >> 16);
    out[2] = (uint8_t)(rgba_pm >> 8);
    out[3] = (uint8_t)rgba_pm;
}

static bool nk_ui_vk_clip_empty(const int clip[4]) {
    return clip[2] <= clip[0] || clip[3] <= clip[1];
}

static int nk_ui_vk_round(float value) {
    return (int)floorf(value + 0.5f);
}

static uint32_t nk_ui_vk_memory_type(const NkUiVk *vk, uint32_t bits,
                                     VkMemoryPropertyFlags want,
                                     VkMemoryPropertyFlags prefer) {
    uint32_t pass;
    for (pass = 0; pass < 2u; ++pass) {
        VkMemoryPropertyFlags flags = pass == 0 ? (want | prefer) : want;
        uint32_t i;
        for (i = 0; i < vk->memory.memoryTypeCount; ++i) {
            if ((bits & (1u << i)) != 0u &&
                (vk->memory.memoryTypes[i].propertyFlags & flags) == flags) {
                return i;
            }
        }
    }
    return UINT32_MAX;
}

/* ---- buffers ---- */

static void nk_ui_vk_buffer_destroy(NkUiVk *vk, NkUiVkBuffer *buffer) {
    if (buffer->mapped != NULL) {
        vk->fn.UnmapMemory(vk->device, buffer->memory);
    }
    if (buffer->buffer != VK_NULL_HANDLE) {
        vk->fn.DestroyBuffer(vk->device, buffer->buffer, vk->allocator);
    }
    if (buffer->memory != VK_NULL_HANDLE) {
        vk->fn.FreeMemory(vk->device, buffer->memory, vk->allocator);
    }
    memset(buffer, 0, sizeof(*buffer));
}

/* Makes `buffer` hold at least `needed` bytes, host visible and mapped. Only a
   slot whose GPU work has finished is grown, and before anything of this frame
   refers to it, so the old buffer is freed at once. */
static bool nk_ui_vk_buffer_reserve(NkUiVk *vk, NkUiVkBuffer *buffer, VkDeviceSize needed,
                                    VkBufferUsageFlags usage) {
    VkBufferCreateInfo info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocate;
    VkDeviceSize size = NK_UI_VK_MIN_BUFFER;
    uint32_t type;
    if (buffer->buffer != VK_NULL_HANDLE && buffer->size >= needed) {
        return true;
    }
    nk_ui_vk_buffer_destroy(vk, buffer);
    while (size < needed) {
        if (size > (VkDeviceSize)UINT64_MAX / 2u) {
            return false;
        }
        size *= 2u;
    }
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vk->fn.CreateBuffer(vk->device, &info, vk->allocator, &buffer->buffer) != VK_SUCCESS) {
        buffer->buffer = VK_NULL_HANDLE;
        return false;
    }
    vk->fn.GetBufferMemoryRequirements(vk->device, buffer->buffer, &requirements);
    type = nk_ui_vk_memory_type(vk, requirements.memoryTypeBits,
                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) {
        nk_ui_vk_buffer_destroy(vk, buffer);
        return false;
    }
    memset(&allocate, 0, sizeof(allocate));
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (vk->fn.AllocateMemory(vk->device, &allocate, vk->allocator, &buffer->memory) != VK_SUCCESS) {
        buffer->memory = VK_NULL_HANDLE;
        nk_ui_vk_buffer_destroy(vk, buffer);
        return false;
    }
    if (vk->fn.BindBufferMemory(vk->device, buffer->buffer, buffer->memory, 0) != VK_SUCCESS ||
        vk->fn.MapMemory(vk->device, buffer->memory, 0, VK_WHOLE_SIZE, 0,
                         &buffer->mapped) != VK_SUCCESS) {
        buffer->mapped = NULL;
        nk_ui_vk_buffer_destroy(vk, buffer);
        return false;
    }
    buffer->size = size;
    buffer->coherent = (vk->memory.memoryTypes[type].propertyFlags &
                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u;
    return true;
}

static bool nk_ui_vk_buffer_flush(NkUiVk *vk, const NkUiVkBuffer *buffer) {
    VkMappedMemoryRange range;
    if (buffer->coherent || buffer->memory == VK_NULL_HANDLE) {
        return true;
    }
    memset(&range, 0, sizeof(range));
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = buffer->memory;
    range.offset = 0;
    range.size = VK_WHOLE_SIZE;
    return vk->fn.FlushMappedMemoryRanges(vk->device, 1, &range) == VK_SUCCESS;
}

/* ---- textures ---- */

static bool nk_ui_vk_allocate_set(NkUiVk *vk, VkDescriptorSet *set, VkDescriptorPool *pool) {
    VkDescriptorSetAllocateInfo info;
    VkDescriptorPoolCreateInfo pool_info;
    VkDescriptorPoolSize size;
    VkDescriptorPool created = VK_NULL_HANDLE;
    size_t i;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorSetCount = 1;
    info.pSetLayouts = &vk->set_layout;
    /* Newest pool first; a full one says so and the next is tried. */
    for (i = vk->pool_count; i > 0; --i) {
        info.descriptorPool = vk->pools[i - 1u];
        if (vk->fn.AllocateDescriptorSets(vk->device, &info, set) == VK_SUCCESS) {
            *pool = vk->pools[i - 1u];
            return true;
        }
    }
    if (!nk_ui_vk_grow((void **)&vk->pools, &vk->pool_capacity, vk->pool_count + 1u,
                       sizeof(*vk->pools))) {
        return false;
    }
    size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    size.descriptorCount = NK_UI_VK_POOL_SETS;
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    pool_info.maxSets = NK_UI_VK_POOL_SETS;
    pool_info.poolSizeCount = 1;
    pool_info.pPoolSizes = &size;
    if (vk->fn.CreateDescriptorPool(vk->device, &pool_info, vk->allocator, &created) != VK_SUCCESS) {
        return false;
    }
    vk->pools[vk->pool_count++] = created;
    info.descriptorPool = created;
    if (vk->fn.AllocateDescriptorSets(vk->device, &info, set) != VK_SUCCESS) {
        return false;
    }
    *pool = created;
    return true;
}

static void nk_ui_vk_texture_destroy(NkUiVk *vk, NkUiVkTexture *texture) {
    if (texture == NULL) {
        return;
    }
    if (texture->set != VK_NULL_HANDLE) {
        vk->fn.FreeDescriptorSets(vk->device, texture->pool, 1, &texture->set);
    }
    if (texture->view != VK_NULL_HANDLE) {
        vk->fn.DestroyImageView(vk->device, texture->view, vk->allocator);
    }
    if (texture->image != VK_NULL_HANDLE) {
        vk->fn.DestroyImage(vk->device, texture->image, vk->allocator);
    }
    if (texture->memory != VK_NULL_HANDLE) {
        vk->fn.FreeMemory(vk->device, texture->memory, vk->allocator);
    }
    free(texture);
}

/* Hands a texture that frames in flight may still read to the garbage list of
   the newest frame slot; it is freed when that slot is prepared again. Never
   allocates, so retiring cannot fail. */
static void nk_ui_vk_retire(NkUiVk *vk, NkUiVkTexture *texture) {
    if (texture == NULL) {
        return;
    }
    if (vk->retire_slot < 0) {
        nk_ui_vk_texture_destroy(vk, texture);
        return;
    }
    texture->next = vk->frames[vk->retire_slot].garbage;
    vk->frames[vk->retire_slot].garbage = texture;
}

static void nk_ui_vk_collect(NkUiVk *vk, NkUiVkFrame *frame) {
    while (frame->garbage != NULL) {
        NkUiVkTexture *next = frame->garbage->next;
        nk_ui_vk_texture_destroy(vk, frame->garbage);
        frame->garbage = next;
    }
}

/* A sampled 2D texture. `coverage` views a single-channel image as (r, r, r, r):
   the glyph atlas's premultiplied white. Its contents are undefined until an
   upload is queued and recorded. */
static NkUiVkTexture *nk_ui_vk_texture_create(NkUiVk *vk, int width, int height,
                                              VkFormat format, bool coverage,
                                              VkSampler sampler) {
    NkUiVkTexture *texture;
    VkImageCreateInfo image_info;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocate;
    VkImageViewCreateInfo view_info;
    VkDescriptorImageInfo descriptor;
    VkWriteDescriptorSet write;
    uint32_t type;
    if (width <= 0 || height <= 0 || width > vk->image_max_side || height > vk->image_max_side) {
        return NULL;
    }
    texture = (NkUiVkTexture *)calloc(1, sizeof(*texture));
    if (texture == NULL) {
        return NULL;
    }
    texture->width = width;
    texture->height = height;
    memset(&image_info, 0, sizeof(image_info));
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = format;
    image_info.extent.width = (uint32_t)width;
    image_info.extent.height = (uint32_t)height;
    image_info.extent.depth = 1;
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vk->fn.CreateImage(vk->device, &image_info, vk->allocator, &texture->image) != VK_SUCCESS) {
        texture->image = VK_NULL_HANDLE;
        nk_ui_vk_texture_destroy(vk, texture);
        return NULL;
    }
    vk->fn.GetImageMemoryRequirements(vk->device, texture->image, &requirements);
    type = nk_ui_vk_memory_type(vk, requirements.memoryTypeBits, 0,
                                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    memset(&allocate, 0, sizeof(allocate));
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (type == UINT32_MAX ||
        vk->fn.AllocateMemory(vk->device, &allocate, vk->allocator, &texture->memory) != VK_SUCCESS) {
        texture->memory = VK_NULL_HANDLE;
        nk_ui_vk_texture_destroy(vk, texture);
        return NULL;
    }
    if (vk->fn.BindImageMemory(vk->device, texture->image, texture->memory, 0) != VK_SUCCESS) {
        nk_ui_vk_texture_destroy(vk, texture);
        return NULL;
    }
    memset(&view_info, 0, sizeof(view_info));
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = texture->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = format;
    if (coverage) {
        view_info.components.r = VK_COMPONENT_SWIZZLE_R;
        view_info.components.g = VK_COMPONENT_SWIZZLE_R;
        view_info.components.b = VK_COMPONENT_SWIZZLE_R;
        view_info.components.a = VK_COMPONENT_SWIZZLE_R;
    } else {
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
    }
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    if (vk->fn.CreateImageView(vk->device, &view_info, vk->allocator, &texture->view) != VK_SUCCESS) {
        texture->view = VK_NULL_HANDLE;
        nk_ui_vk_texture_destroy(vk, texture);
        return NULL;
    }
    if (!nk_ui_vk_allocate_set(vk, &texture->set, &texture->pool)) {
        texture->set = VK_NULL_HANDLE;
        nk_ui_vk_texture_destroy(vk, texture);
        return NULL;
    }
    descriptor.sampler = sampler;
    descriptor.imageView = texture->view;
    descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    memset(&write, 0, sizeof(write));
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = texture->set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descriptor;
    vk->fn.UpdateDescriptorSets(vk->device, 1, &write, 0, NULL);
    return texture;
}

/* Queues an upload into `texture`, recorded at the end of this prepare: a clear
   of the whole image when `clear`, and the w x h rectangle at (x, y) from `src`
   (rows of row_bytes, src_pitch apart) when `src` is not NULL. All or nothing:
   false means no state changed. */
static bool nk_ui_vk_queue_upload(NkUiVk *vk, NkUiVkTexture *texture, bool clear,
                                  int x, int y, int w, int h, const uint8_t *src,
                                  size_t src_pitch, size_t row_bytes) {
    bool new_touch = texture->touch_serial != vk->serial;
    size_t offset = 0;
    size_t bytes = 0;
    if (src != NULL) {
        if (w <= 0 || h <= 0 || row_bytes > SIZE_MAX / (size_t)h) {
            return false;
        }
        bytes = row_bytes * (size_t)h;
        offset = (vk->upload_size + NK_UI_VK_UPLOAD_ALIGN - 1u) & ~(NK_UI_VK_UPLOAD_ALIGN - 1u);
        if (offset < vk->upload_size || bytes > SIZE_MAX - offset ||
            !nk_ui_vk_grow((void **)&vk->upload, &vk->upload_capacity, offset + bytes, 1u)) {
            return false;
        }
        if (!nk_ui_vk_grow((void **)&vk->copies, &vk->copy_capacity, vk->copy_count + 1u,
                           sizeof(*vk->copies)) ||
            !nk_ui_vk_grow((void **)&vk->regions, &vk->region_capacity, vk->copy_count + 1u,
                           sizeof(*vk->regions))) {
            return false;
        }
    }
    if (new_touch) {
        if (!nk_ui_vk_grow((void **)&vk->touched, &vk->touched_capacity, vk->touched_count + 1u,
                           sizeof(*vk->touched)) ||
            !nk_ui_vk_grow((void **)&vk->barriers, &vk->barrier_capacity, vk->touched_count + 1u,
                           sizeof(*vk->barriers))) {
            return false;
        }
        texture->touch_serial = vk->serial;
        texture->touch_index = vk->touched_count;
        vk->touched[vk->touched_count].image = texture->image;
        vk->touched[vk->touched_count].was_initialized = texture->initialized;
        vk->touched[vk->touched_count].clear = false;
        ++vk->touched_count;
    }
    if (clear) {
        vk->touched[texture->touch_index].clear = true;
    }
    if (src != NULL) {
        NkUiVkCopy *copy = &vk->copies[vk->copy_count++];
        int row;
        for (row = 0; row < h; ++row) {
            memcpy(vk->upload + offset + (size_t)row * row_bytes,
                   src + (size_t)row * src_pitch, row_bytes);
        }
        vk->upload_size = offset + bytes;
        copy->touched = texture->touch_index;
        copy->offset = (VkDeviceSize)offset;
        copy->x = x;
        copy->y = y;
        copy->w = w;
        copy->h = h;
    }
    return true;
}

/* ---- atlas ---- */

static uint32_t nk_ui_vk_hash(int font, int px, uint32_t glyph) {
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)font) * 16777619u;
    h = (h ^ (uint32_t)px) * 16777619u;
    h = (h ^ glyph) * 16777619u;
    h ^= h >> 15;
    return h;
}

static void nk_ui_vk_drop_atlas(NkUiVk *vk) {
    int i;
    for (i = 0; i < vk->page_count; ++i) {
        nk_ui_vk_retire(vk, vk->pages[i].texture);
    }
    memset(vk->pages, 0, sizeof(vk->pages));
    vk->page_count = 0;
    vk->entry_count = 0;
    if (vk->table != NULL) {
        size_t slot;
        for (slot = 0; slot < vk->table_capacity; ++slot) {
            vk->table[slot] = -1;
        }
    }
}

static bool nk_ui_vk_table_grow(NkUiVk *vk) {
    size_t capacity = vk->table_capacity == 0 ? 512u : vk->table_capacity * 2u;
    int32_t *table = (int32_t *)malloc(capacity * sizeof(*table));
    size_t i;
    if (table == NULL) {
        return false;
    }
    for (i = 0; i < capacity; ++i) {
        table[i] = -1;
    }
    for (i = 0; i < vk->entry_count; ++i) {
        size_t slot = nk_ui_vk_hash(vk->entries[i].font, vk->entries[i].px,
                                    vk->entries[i].glyph) & (capacity - 1u);
        while (table[slot] >= 0) {
            slot = (slot + 1u) & (capacity - 1u);
        }
        table[slot] = (int32_t)i;
    }
    free(vk->table);
    vk->table = table;
    vk->table_capacity = capacity;
    return true;
}

static NkUiVkEntry *nk_ui_vk_find(NkUiVk *vk, int font, int px, uint32_t glyph) {
    size_t slot;
    if (vk->table_capacity == 0) {
        return NULL;
    }
    slot = nk_ui_vk_hash(font, px, glyph) & (vk->table_capacity - 1u);
    while (vk->table[slot] >= 0) {
        NkUiVkEntry *entry = &vk->entries[vk->table[slot]];
        if (entry->font == font && entry->px == px && entry->glyph == glyph) {
            return entry;
        }
        slot = (slot + 1u) & (vk->table_capacity - 1u);
    }
    return NULL;
}

static NkUiVkEntry *nk_ui_vk_add_entry(NkUiVk *vk, int font, int px, uint32_t glyph) {
    NkUiVkEntry *entry;
    size_t slot;
    if ((vk->entry_count + 1u) * 2u > vk->table_capacity && !nk_ui_vk_table_grow(vk)) {
        return NULL;
    }
    if (vk->entry_count >= (size_t)INT32_MAX ||
        !nk_ui_vk_grow((void **)&vk->entries, &vk->entry_capacity, vk->entry_count + 1u,
                       sizeof(*vk->entries))) {
        return NULL;
    }
    entry = &vk->entries[vk->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->font = font;
    entry->px = px;
    entry->glyph = glyph;
    entry->page = -1;
    slot = nk_ui_vk_hash(font, px, glyph) & (vk->table_capacity - 1u);
    while (vk->table[slot] >= 0) {
        slot = (slot + 1u) & (vk->table_capacity - 1u);
    }
    vk->table[slot] = (int32_t)vk->entry_count;
    ++vk->entry_count;
    return entry;
}

/* Finds room for a w x h cell (padding included) and returns its page; -1 when
   the cell is larger than a page, -2 when every page is full (the caller drops
   the atlas and tries again), -3 when a new page cannot be made. */
static int nk_ui_vk_place(NkUiVk *vk, int w, int h, int *x, int *y) {
    int cell_w = w + NK_UI_VK_PAD;
    int cell_h = h + NK_UI_VK_PAD;
    int attempt;
    if (cell_w > vk->page_size || cell_h > vk->page_size) {
        return -1;
    }
    for (attempt = 0; attempt < 2; ++attempt) {
        if (vk->page_count > 0) {
            NkUiVkPage *page = &vk->pages[vk->page_count - 1];
            if (page->cursor_x + cell_w > vk->page_size) {
                page->cursor_x = 0;
                page->cursor_y += page->row_height;
                page->row_height = 0;
            }
            if (page->cursor_y + cell_h <= vk->page_size) {
                *x = page->cursor_x;
                *y = page->cursor_y;
                page->cursor_x += cell_w;
                if (cell_h > page->row_height) {
                    page->row_height = cell_h;
                }
                return vk->page_count - 1;
            }
        }
        if (vk->page_count == NK_UI_VK_MAX_PAGES) {
            return -2;
        }
        {
            NkUiVkPage *page = &vk->pages[vk->page_count];
            NkUiVkTexture *texture = nk_ui_vk_texture_create(vk, vk->page_size, vk->page_size,
                                                             VK_FORMAT_R8_UNORM, true,
                                                             vk->nearest);
            if (texture == NULL) {
                return -3;
            }
            /* A new page starts cleared, so no texel is ever undefined. */
            if (!nk_ui_vk_queue_upload(vk, texture, true, 0, 0, 0, 0, NULL, 0, 0)) {
                nk_ui_vk_texture_destroy(vk, texture);
                return -3;
            }
            vk->atlas_pending = true;
            page->texture = texture;
            page->cursor_x = 0;
            page->cursor_y = 0;
            page->row_height = 0;
            ++vk->page_count;
        }
    }
    return -1;
}

/* Returns the cached entry for a glyph, rasterising it and queueing its upload
   on first use. A full atlas is dropped and started again: the pages it drops
   stay alive until this frame is done, so glyphs this frame already placed on
   them still draw. Returns NULL only on allocation or Vulkan failure. */
static NkUiVkEntry *nk_ui_vk_glyph(NkUiVk *vk, int font, int px, uint32_t glyph) {
    NkUiVkEntry *entry = nk_ui_vk_find(vk, font, px, glyph);
    NkUiGlyphBitmap bitmap;
    int page;
    int x = 0;
    int y = 0;
    if (entry != NULL) {
        return entry;
    }
    memset(&bitmap, 0, sizeof(bitmap));
    if (vk->text.raster == NULL ||
        !vk->text.raster(vk->text.user, font, px, glyph, &bitmap) ||
        bitmap.pixels == NULL || bitmap.width <= 0 || bitmap.height <= 0 ||
        bitmap.pitch < bitmap.width) {
        /* Blank glyphs and failures are remembered, so they cost one call. */
        return nk_ui_vk_add_entry(vk, font, px, glyph);
    }
    page = nk_ui_vk_place(vk, bitmap.width, bitmap.height, &x, &y);
    if (page == -2) {
        ++vk->stats.atlas_rebuilds;
        nk_ui_vk_drop_atlas(vk);
        page = nk_ui_vk_place(vk, bitmap.width, bitmap.height, &x, &y);
    }
    if (page == -3) {
        return NULL;
    }
    if (page >= 0 &&
        !nk_ui_vk_queue_upload(vk, vk->pages[page].texture, false, x, y, bitmap.width,
                               bitmap.height, bitmap.pixels, (size_t)bitmap.pitch,
                               (size_t)bitmap.width)) {
        return NULL;
    }
    entry = nk_ui_vk_add_entry(vk, font, px, glyph);
    if (entry == NULL) {
        return NULL;
    }
    if (page < 0) {
        return entry; /* larger than a page: skipped, cached */
    }
    vk->atlas_pending = true;
    ++vk->delta.atlas_uploads;
    entry->page = page;
    entry->x = x;
    entry->y = y;
    entry->w = bitmap.width;
    entry->h = bitmap.height;
    entry->bearing_x = bitmap.bearing_x;
    entry->bearing_y = bitmap.bearing_y;
    return entry;
}

/* ---- frame geometry ---- */

/* Room for `quads` more quads in the frame's own vertices and indices. */
static bool nk_ui_vk_reserve_quads(NkUiVk *vk, size_t quads) {
    size_t vertices = (size_t)vk->list->nv + vk->extra_vertex_count;
    size_t indices = (size_t)vk->list->ni + vk->extra_index_count;
    if (quads > (size_t)UINT32_MAX / 6u || vertices + quads * 4u > (size_t)UINT32_MAX ||
        indices + quads * 6u > (size_t)UINT32_MAX) {
        return false;
    }
    return nk_ui_vk_grow((void **)&vk->extra_vertices, &vk->extra_vertex_capacity,
                         (size_t)vk->extra_vertex_count + quads * 4u,
                         sizeof(*vk->extra_vertices)) &&
           nk_ui_vk_grow((void **)&vk->extra_indices, &vk->extra_index_capacity,
                         (size_t)vk->extra_index_count + quads * 6u,
                         sizeof(*vk->extra_indices));
}

/* Appends one quad (room reserved) and returns the index-buffer position of
   its first index. */
static uint32_t nk_ui_vk_push_quad(NkUiVk *vk, float x0, float y0, float x1, float y1,
                                   float u0, float v0, float u1, float v1,
                                   uint32_t rgba_pm) {
    NkUiVkVertex *q = vk->extra_vertices + vk->extra_vertex_count;
    uint32_t *index = vk->extra_indices + vk->extra_index_count;
    uint32_t base = vk->list->nv + vk->extra_vertex_count;
    uint32_t first = vk->list->ni + vk->extra_index_count;
    int k;
    q[0].x = x0; q[0].y = y0; q[0].u = u0; q[0].v = v0;
    q[1].x = x1; q[1].y = y0; q[1].u = u1; q[1].v = v0;
    q[2].x = x1; q[2].y = y1; q[2].u = u1; q[2].v = v1;
    q[3].x = x0; q[3].y = y1; q[3].u = u0; q[3].v = v1;
    for (k = 0; k < 4; ++k) {
        nk_ui_vk_color(q[k].rgba, rgba_pm);
    }
    index[0] = base;
    index[1] = base + 1u;
    index[2] = base + 2u;
    index[3] = base;
    index[4] = base + 2u;
    index[5] = base + 3u;
    vk->extra_vertex_count += 4u;
    vk->extra_index_count += 6u;
    return first;
}

/* Adds a draw, folded into the previous one when it continues it: same
   texture, same clip, the next indices. */
static bool nk_ui_vk_add_draw(NkUiVk *vk, uint32_t first, uint32_t count, VkDescriptorSet set,
                              const int clip[4]) {
    NkUiVkDraw *draw;
    if (count == 0) {
        return true;
    }
    if (vk->draw_count > 0) {
        draw = &vk->draws[vk->draw_count - 1u];
        if (draw->set == set && memcmp(draw->clip, clip, sizeof(draw->clip)) == 0 &&
            draw->first + draw->count == first && count <= UINT32_MAX - draw->count) {
            draw->count += count;
            return true;
        }
    }
    if (!nk_ui_vk_grow((void **)&vk->draws, &vk->draw_capacity, vk->draw_count + 1u,
                       sizeof(*vk->draws))) {
        return false;
    }
    draw = &vk->draws[vk->draw_count++];
    draw->first = first;
    draw->count = count;
    draw->set = set;
    memcpy(draw->clip, clip, sizeof(draw->clip));
    return true;
}

static bool nk_ui_vk_prepare_text(NkUiVk *vk, const NkUiDrawCmd *cmd) {
    const NkUiTextRun *run = (const NkUiTextRun *)cmd->custom;
    NkUiTextSize size;
    size_t needed;
    int count;
    int i;
    int origin_x;
    int baseline_y;
    float inverse;
    if (run == NULL || run->text == NULL || run->length == 0 || run->size_px <= 0 ||
        vk->text.shape == NULL || vk->text.raster == NULL) {
        return true;
    }
    needed = run->length < NK_UI_VK_MAX_GLYPHS_PER_RUN ? run->length : NK_UI_VK_MAX_GLYPHS_PER_RUN;
    if (!nk_ui_vk_grow((void **)&vk->glyphs, &vk->glyph_capacity, needed, sizeof(*vk->glyphs)) ||
        !nk_ui_vk_reserve_quads(vk, needed)) {
        return false;
    }
    count = vk->text.shape(vk->text.user, run->font, run->size_px, run->text, run->length,
                           vk->glyphs, (int)needed);
    if (count <= 0) {
        return true;
    }
    if ((size_t)count > needed) {
        count = (int)needed;
    }
    memset(&size, 0, sizeof(size));
    if (vk->text.measure != NULL) {
        size = vk->text.measure(vk->text.user, run->font, run->size_px, run->text, run->length);
    }
    /* Centre the face's line box in the command's bounds (CSS half-leading),
       then snap: the baseline is a whole pixel row. */
    origin_x = cmd->bounds[0];
    if (run->center) {
        origin_x += nk_ui_vk_round(((float)(cmd->bounds[2] - cmd->bounds[0]) - size.width) * 0.5f);
    }
    baseline_y = cmd->bounds[1] +
                 nk_ui_vk_round(((float)(cmd->bounds[3] - cmd->bounds[1]) - size.height) * 0.5f) +
                 nk_ui_vk_round(size.baseline);
    inverse = 1.0f / (float)vk->page_size;
    for (i = 0; i < count; ++i) {
        const NkUiGlyph *glyph = &vk->glyphs[i];
        NkUiVkEntry *entry = nk_ui_vk_glyph(vk, run->font, run->size_px, glyph->glyph);
        float x0;
        float y0;
        uint32_t first;
        if (entry == NULL) {
            return false;
        }
        if (entry->page < 0) {
            continue;
        }
        x0 = (float)(origin_x + nk_ui_vk_round(glyph->x + run->letter_spacing * (float)i) +
                     entry->bearing_x);
        y0 = (float)(baseline_y + nk_ui_vk_round(glyph->y) - entry->bearing_y);
        first = nk_ui_vk_push_quad(vk, x0, y0, x0 + (float)entry->w, y0 + (float)entry->h,
                                   (float)entry->x * inverse, (float)entry->y * inverse,
                                   (float)(entry->x + entry->w) * inverse,
                                   (float)(entry->y + entry->h) * inverse, cmd->rgba_pm);
        if (!nk_ui_vk_add_draw(vk, first, 6u, vk->pages[entry->page].texture->set, cmd->clip)) {
            return false;
        }
        ++vk->delta.glyphs_drawn;
    }
    ++vk->delta.text_runs_drawn;
    return true;
}

/* The texture for an image, created and its upload queued on first use. A full
   cache gives up the entry drawn longest ago; one drawn in this frame is never
   evicted, so an image beyond that gets a texture for this frame only. NULL
   when no texture could be made (the image is then left out). */
static NkUiVkTexture *nk_ui_vk_image_texture(NkUiVk *vk, const NkUiImage *image,
                                             int width, int height) {
    NkUiVkImageEntry *slot = NULL;
    NkUiVkTexture *texture;
    uint8_t *shrunk = NULL;
    const uint8_t *pixels = image->rgba_pm;
    size_t i;
    for (i = 0; i < NK_UI_VK_IMAGE_CACHE; ++i) {
        if (vk->images[i].serial == image->serial && vk->images[i].texture != NULL &&
            vk->images[i].width == width && vk->images[i].height == height) {
            vk->images[i].last_used = vk->serial;
            return vk->images[i].texture;
        }
    }
    for (i = 0; i < NK_UI_VK_IMAGE_CACHE; ++i) {
        NkUiVkImageEntry *entry = &vk->images[i];
        if (entry->texture == NULL) {
            slot = entry;
            break;
        }
        if (entry->last_used != vk->serial &&
            (slot == NULL || entry->last_used < slot->last_used)) {
            slot = entry;
        }
    }
    if (width != (int)image->width || height != (int)image->height) {
        shrunk = (uint8_t *)malloc((size_t)width * (size_t)height * 4u);
        if (shrunk == NULL ||
            !nk_ui_image_resample(image, (uint32_t)width, (uint32_t)height, shrunk)) {
            free(shrunk);
            return NULL;
        }
        pixels = shrunk;
    }
    texture = nk_ui_vk_texture_create(vk, width, height, VK_FORMAT_R8G8B8A8_UNORM, false,
                                      vk->linear);
    if (texture == NULL ||
        !nk_ui_vk_queue_upload(vk, texture, false, 0, 0, width, height, pixels,
                               (size_t)width * 4u, (size_t)width * 4u)) {
        nk_ui_vk_texture_destroy(vk, texture);
        free(shrunk);
        return NULL;
    }
    free(shrunk);
    ++vk->delta.image_uploads;
    if (slot == NULL) {
        /* Drawn this frame only: freed with the frame's slot. */
        nk_ui_vk_retire(vk, texture);
        return texture;
    }
    nk_ui_vk_retire(vk, slot->texture);
    slot->serial = image->serial;
    slot->width = width;
    slot->height = height;
    slot->last_used = vk->serial;
    slot->texture = texture;
    return texture;
}

static bool nk_ui_vk_prepare_image(NkUiVk *vk, const NkUiDrawCmd *cmd) {
    const NkUiImage *image = (const NkUiImage *)cmd->custom;
    NkUiVkTexture *texture;
    uint32_t first;
    int width;
    int height;
    if (image == NULL || image->rgba_pm == NULL || image->width == 0u || image->height == 0u ||
        image->width > (uint32_t)NK_UI_VK_IMAGE_MAX_SIDE ||
        image->height > (uint32_t)NK_UI_VK_IMAGE_MAX_SIDE ||
        cmd->bounds[2] <= cmd->bounds[0] || cmd->bounds[3] <= cmd->bounds[1]) {
        return true;
    }
    /* Shrunk by more than a quarter: upload an area-averaged copy at the drawn
       size, so a 64 px glyph drawn at 24 px does not alias. */
    width = (int)image->width;
    height = (int)image->height;
    if ((cmd->bounds[2] - cmd->bounds[0]) * 5 < width * 4 ||
        (cmd->bounds[3] - cmd->bounds[1]) * 5 < height * 4) {
        width = cmd->bounds[2] - cmd->bounds[0];
        height = cmd->bounds[3] - cmd->bounds[1];
    }
    if (!nk_ui_vk_reserve_quads(vk, 1u)) {
        return false;
    }
    texture = nk_ui_vk_image_texture(vk, image, width, height);
    if (texture == NULL) {
        ++vk->delta.images_failed;
        return true; /* an image that cannot upload is left out, never fatal */
    }
    first = nk_ui_vk_push_quad(vk, (float)cmd->bounds[0], (float)cmd->bounds[1],
                               (float)cmd->bounds[2], (float)cmd->bounds[3],
                               0.0f, 0.0f, 1.0f, 1.0f, cmd->rgba_pm);
    if (!nk_ui_vk_add_draw(vk, first, 6u, texture->set, cmd->clip)) {
        return false;
    }
    ++vk->delta.images_drawn;
    return true;
}

/* One waves command: its frame is rendered on the CPU again only when its
   phases, size or palette changed, so a held-still frame costs one quad. */
static bool nk_ui_vk_prepare_waves(NkUiVk *vk, const NkUiDrawCmd *cmd) {
    const NkUiWavesCommand *command = (const NkUiWavesCommand *)cmd->custom;
    int bw = cmd->bounds[2] - cmd->bounds[0];
    int bh = cmd->bounds[3] - cmd->bounds[1];
    NkUiVkWaves *waves;
    uint32_t first;
    int w;
    int h;
    if (command == NULL || bw <= 0 || bh <= 0) {
        return true;
    }
    if (vk->waves_used >= NK_UI_VK_WAVES_SLOTS || !nk_ui_vk_reserve_quads(vk, 1u)) {
        return false;
    }
    waves = &vk->waves[vk->waves_used++];
    w = (bw + NK_UI_VK_WAVES_DIVISOR - 1) / NK_UI_VK_WAVES_DIVISOR;
    if (w > NK_UI_VK_WAVES_MAX_W) {
        w = NK_UI_VK_WAVES_MAX_W;
    }
    h = (int)((long long)bh * w / bw);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (waves->texture == NULL || waves->w != w || waves->h != h) {
        uint8_t *pixels;
        nk_ui_vk_retire(vk, waves->texture);
        waves->texture = NULL;
        waves->w = 0;
        waves->h = 0;
        waves->valid = false;
        pixels = (uint8_t *)realloc(waves->pixels, (size_t)w * (size_t)h * 4u);
        if (pixels == NULL) {
            return false;
        }
        waves->pixels = pixels;
        waves->texture = nk_ui_vk_texture_create(vk, w, h, VK_FORMAT_R8G8B8A8_UNORM, false,
                                                 vk->linear);
        if (waves->texture == NULL) {
            return false;
        }
        waves->w = w;
        waves->h = h;
    }
    if (!waves->valid || memcmp(&waves->last, command, sizeof(*command)) != 0) {
        waves->valid = false;
        if (!nk_ui_waves_render_command(command, (uint32_t)w, (uint32_t)h, waves->pixels,
                                        (size_t)w * 4u) ||
            !nk_ui_vk_queue_upload(vk, waves->texture, false, 0, 0, w, h, waves->pixels,
                                   (size_t)w * 4u, (size_t)w * 4u)) {
            return false;
        }
        waves->last = *command;
        waves->valid = true;
        ++vk->delta.waves_rendered;
    }
    first = nk_ui_vk_push_quad(vk, (float)cmd->bounds[0], (float)cmd->bounds[1],
                               (float)cmd->bounds[2], (float)cmd->bounds[3],
                               0.0f, 0.0f, 1.0f, 1.0f, 0xFFFFFFFFu);
    if (!nk_ui_vk_add_draw(vk, first, 6u, waves->texture->set, cmd->clip)) {
        return false;
    }
    ++vk->delta.waves_drawn;
    return true;
}

/* Every index names a vertex, every geometry command's range is inside the
   index buffer, and scissors nest no deeper than NK_UI_DRAW_CLIP_DEPTH, with
   no end before its start. */
static bool nk_ui_vk_list_valid(const NkUiDrawList *list) {
    uint32_t depth = 0;
    uint32_t i;
    if ((list->nv > 0 && list->v == NULL) || (list->ni > 0 && list->idx == NULL) ||
        (list->ncmd > 0 && list->cmd == NULL)) {
        return false;
    }
    for (i = 0; i < list->ni; ++i) {
        if (list->idx[i] >= list->nv) {
            return false;
        }
    }
    for (i = 0; i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_RECTANGLE || cmd->kind == NK_UI_DRAW_BORDER) {
            if (cmd->first > list->ni || cmd->count > list->ni - cmd->first) {
                return false;
            }
        } else if (cmd->kind == NK_UI_DRAW_SCISSOR_START) {
            if (depth == NK_UI_DRAW_CLIP_DEPTH) {
                return false;
            }
            ++depth;
        } else if (cmd->kind == NK_UI_DRAW_SCISSOR_END) {
            if (depth == 0) {
                return false;
            }
            --depth;
        }
    }
    return true;
}

/* Undoes what a failed prepare left half done. Nothing was recorded, so every
   texture it created or queued an upload for is either dropped or marked to be
   uploaded again. */
static void nk_ui_vk_rollback(NkUiVk *vk) {
    size_t i;
    if (vk->atlas_pending) {
        if (vk->page_count > 0 || vk->entry_count > 0) {
            ++vk->stats.atlas_rebuilds;
        }
        nk_ui_vk_drop_atlas(vk);
    }
    for (i = 0; i < NK_UI_VK_IMAGE_CACHE; ++i) {
        if (vk->images[i].texture != NULL && !vk->images[i].texture->initialized) {
            nk_ui_vk_retire(vk, vk->images[i].texture);
            memset(&vk->images[i], 0, sizeof(vk->images[i]));
        }
    }
    for (i = 0; i < NK_UI_VK_WAVES_SLOTS; ++i) {
        NkUiVkWaves *waves = &vk->waves[i];
        if (waves->texture != NULL && waves->texture->touch_serial == vk->serial) {
            waves->valid = false;
        }
        if (waves->texture != NULL && !waves->texture->initialized) {
            nk_ui_vk_retire(vk, waves->texture);
            waves->texture = NULL;
            waves->w = 0;
            waves->h = 0;
            waves->valid = false;
        }
    }
}

static void nk_ui_vk_mark_initialized(NkUiVk *vk, NkUiVkTexture *texture) {
    if (texture != NULL && texture->touch_serial == vk->serial) {
        texture->initialized = true;
    }
}

/* Records every queued upload: each touched image to TRANSFER_DST (from what
   frames before left it in), its clear, its copies, then to SHADER_READ_ONLY
   for the fragment shader. A pipeline barrier also orders these writes after
   every earlier command submitted to the queue, so a frame in flight that
   still samples the old contents finishes first. */
static void nk_ui_vk_record_uploads(NkUiVk *vk, VkCommandBuffer command_buffer,
                                    VkBuffer staging) {
    VkImageSubresourceRange range;
    bool any_clear = false;
    size_t i;
    if (vk->touched_count == 0) {
        return;
    }
    memset(&range, 0, sizeof(range));
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    for (i = 0; i < vk->touched_count; ++i) {
        VkImageMemoryBarrier *barrier = &vk->barriers[i];
        memset(barrier, 0, sizeof(*barrier));
        barrier->sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier->srcAccessMask = vk->touched[i].was_initialized ? VK_ACCESS_SHADER_READ_BIT : 0u;
        barrier->dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier->oldLayout = vk->touched[i].was_initialized ?
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        barrier->newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier->srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier->dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier->image = vk->touched[i].image;
        barrier->subresourceRange = range;
        any_clear = any_clear || vk->touched[i].clear;
    }
    vk->fn.CmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                              (uint32_t)vk->touched_count, vk->barriers);
    if (any_clear) {
        VkClearColorValue zero;
        VkMemoryBarrier barrier;
        memset(&zero, 0, sizeof(zero));
        for (i = 0; i < vk->touched_count; ++i) {
            if (vk->touched[i].clear) {
                vk->fn.CmdClearColorImage(command_buffer, vk->touched[i].image,
                                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
            }
        }
        /* The copies land on the cleared images: write after write. */
        memset(&barrier, 0, sizeof(barrier));
        barrier.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vk->fn.CmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                  VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier, 0, NULL, 0, NULL);
    }
    i = 0;
    while (i < vk->copy_count) {
        size_t start = i;
        size_t touched = vk->copies[i].touched;
        while (i < vk->copy_count && vk->copies[i].touched == touched) {
            VkBufferImageCopy *region = &vk->regions[i - start];
            memset(region, 0, sizeof(*region));
            region->bufferOffset = vk->copies[i].offset;
            region->imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region->imageSubresource.layerCount = 1;
            region->imageOffset.x = vk->copies[i].x;
            region->imageOffset.y = vk->copies[i].y;
            region->imageExtent.width = (uint32_t)vk->copies[i].w;
            region->imageExtent.height = (uint32_t)vk->copies[i].h;
            region->imageExtent.depth = 1;
            ++i;
        }
        vk->fn.CmdCopyBufferToImage(command_buffer, staging, vk->touched[touched].image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    (uint32_t)(i - start), vk->regions);
    }
    for (i = 0; i < vk->touched_count; ++i) {
        VkImageMemoryBarrier *barrier = &vk->barriers[i];
        barrier->srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier->dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier->oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier->newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    vk->fn.CmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                              VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL,
                              (uint32_t)vk->touched_count, vk->barriers);
}

/* Fills the slot's buffers: the list's vertices (colours as bytes) then the
   frame's quads, the list's indices then the quads', and the staged uploads. */
static bool nk_ui_vk_write_buffers(NkUiVk *vk, NkUiVkFrame *frame) {
    const NkUiDrawList *list = vk->list;
    VkDeviceSize vertex_count = (VkDeviceSize)list->nv + vk->extra_vertex_count;
    VkDeviceSize index_count = (VkDeviceSize)list->ni + vk->extra_index_count;
    if (vertex_count > 0) {
        NkUiVkVertex *out;
        uint32_t i;
        if (!nk_ui_vk_buffer_reserve(vk, &frame->vertices, vertex_count * sizeof(NkUiVkVertex),
                                     VK_BUFFER_USAGE_VERTEX_BUFFER_BIT)) {
            return false;
        }
        out = (NkUiVkVertex *)frame->vertices.mapped;
        for (i = 0; i < list->nv; ++i) {
            out[i].x = list->v[i].x;
            out[i].y = list->v[i].y;
            out[i].u = list->v[i].u;
            out[i].v = list->v[i].v;
            nk_ui_vk_color(out[i].rgba, list->v[i].rgba_pm);
        }
        if (vk->extra_vertex_count > 0) {
            memcpy(out + list->nv, vk->extra_vertices,
                   (size_t)vk->extra_vertex_count * sizeof(*vk->extra_vertices));
        }
        if (!nk_ui_vk_buffer_flush(vk, &frame->vertices)) {
            return false;
        }
    }
    if (index_count > 0) {
        uint32_t *out;
        if (!nk_ui_vk_buffer_reserve(vk, &frame->indices, index_count * sizeof(uint32_t),
                                     VK_BUFFER_USAGE_INDEX_BUFFER_BIT)) {
            return false;
        }
        out = (uint32_t *)frame->indices.mapped;
        if (list->ni > 0) {
            memcpy(out, list->idx, (size_t)list->ni * sizeof(uint32_t));
        }
        if (vk->extra_index_count > 0) {
            memcpy(out + list->ni, vk->extra_indices,
                   (size_t)vk->extra_index_count * sizeof(uint32_t));
        }
        if (!nk_ui_vk_buffer_flush(vk, &frame->indices)) {
            return false;
        }
    }
    if (vk->upload_size > 0) {
        if (!nk_ui_vk_buffer_reserve(vk, &frame->staging, (VkDeviceSize)vk->upload_size,
                                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) {
            return false;
        }
        memcpy(frame->staging.mapped, vk->upload, vk->upload_size);
        if (!nk_ui_vk_buffer_flush(vk, &frame->staging)) {
            return false;
        }
    }
    return true;
}

static bool nk_ui_vk_prepare_commands(NkUiVk *vk) {
    const NkUiDrawList *list = vk->list;
    uint32_t i;
    if (!vk->white->initialized) {
        static const uint8_t white[4] = {0xFFu, 0xFFu, 0xFFu, 0xFFu};
        if (!nk_ui_vk_queue_upload(vk, vk->white, false, 0, 0, 1, 1, white, 4u, 4u)) {
            return false;
        }
    }
    for (i = 0; i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        bool drawing = cmd->kind == NK_UI_DRAW_RECTANGLE || cmd->kind == NK_UI_DRAW_BORDER ||
                       cmd->kind == NK_UI_DRAW_TEXT ||
                       (cmd->kind == NK_UI_DRAW_CUSTOM &&
                        (cmd->custom_kind == NK_UI_CUSTOM_WAVES ||
                         cmd->custom_kind == NK_UI_CUSTOM_IMAGE));
        bool ok = true;
        if (drawing && nk_ui_vk_clip_empty(cmd->clip)) {
            ++vk->delta.commands_clipped;
            continue;
        }
        switch (cmd->kind) {
        case NK_UI_DRAW_RECTANGLE:
        case NK_UI_DRAW_BORDER:
            ok = nk_ui_vk_add_draw(vk, cmd->first, cmd->count, vk->white->set, cmd->clip);
            break;
        case NK_UI_DRAW_TEXT:
            ok = nk_ui_vk_prepare_text(vk, cmd);
            break;
        case NK_UI_DRAW_SCISSOR_START:
        case NK_UI_DRAW_SCISSOR_END:
            /* Already folded into every command's clip. */
            break;
        case NK_UI_DRAW_CUSTOM:
            if (cmd->custom_kind == NK_UI_CUSTOM_WAVES) {
                ok = nk_ui_vk_prepare_waves(vk, cmd);
            } else if (cmd->custom_kind == NK_UI_CUSTOM_IMAGE) {
                ok = nk_ui_vk_prepare_image(vk, cmd);
            } else {
                ++vk->delta.custom_skipped;
            }
            break;
        default:
            ++vk->delta.custom_skipped;
            break;
        }
        if (!ok) {
            return false;
        }
    }
    return true;
}

static void nk_ui_vk_add_stats(NkUiVkStats *total, const NkUiVkStats *delta) {
    total->atlas_uploads += delta->atlas_uploads;
    total->glyphs_drawn += delta->glyphs_drawn;
    total->text_runs_drawn += delta->text_runs_drawn;
    total->custom_skipped += delta->custom_skipped;
    total->waves_drawn += delta->waves_drawn;
    total->waves_rendered += delta->waves_rendered;
    total->images_drawn += delta->images_drawn;
    total->image_uploads += delta->image_uploads;
    total->images_failed += delta->images_failed;
    total->commands_clipped += delta->commands_clipped;
}

bool nk_ui_vk_prepare(NkUiVk *vk, const NkUiDrawList *list,
                      struct VkCommandBuffer_T *command_buffer, uint32_t frame_slot) {
    NkUiVkFrame *frame;
    size_t i;
    if (vk == NULL) {
        return false;
    }
    vk->prepared = false;
    if (list == NULL || command_buffer == NULL || frame_slot >= vk->frames_in_flight) {
        ++vk->stats.frames_failed;
        return false;
    }
    /* The host has waited for this slot's last frame, and with it for every
       frame submitted before that one. */
    frame = &vk->frames[frame_slot];
    nk_ui_vk_collect(vk, frame);
    vk->retire_slot = (int)frame_slot;
    if (!nk_ui_vk_list_valid(list)) {
        ++vk->stats.frames_failed;
        return false;
    }
    ++vk->serial;
    vk->list = list;
    vk->extra_vertex_count = 0;
    vk->extra_index_count = 0;
    vk->draw_count = 0;
    vk->upload_size = 0;
    vk->touched_count = 0;
    vk->copy_count = 0;
    vk->atlas_pending = false;
    vk->waves_used = 0;
    memset(&vk->delta, 0, sizeof(vk->delta));
    if (!nk_ui_vk_prepare_commands(vk) || !nk_ui_vk_write_buffers(vk, frame)) {
        nk_ui_vk_rollback(vk);
        vk->draw_count = 0;
        ++vk->stats.frames_failed;
        return false;
    }
    nk_ui_vk_record_uploads(vk, command_buffer, frame->staging.buffer);
    nk_ui_vk_mark_initialized(vk, vk->white);
    for (i = 0; i < (size_t)vk->page_count; ++i) {
        nk_ui_vk_mark_initialized(vk, vk->pages[i].texture);
    }
    for (i = 0; i < NK_UI_VK_IMAGE_CACHE; ++i) {
        nk_ui_vk_mark_initialized(vk, vk->images[i].texture);
    }
    for (i = 0; i < NK_UI_VK_WAVES_SLOTS; ++i) {
        nk_ui_vk_mark_initialized(vk, vk->waves[i].texture);
    }
    nk_ui_vk_add_stats(&vk->stats, &vk->delta);
    vk->prepared = true;
    vk->prepared_list = list;
    vk->prepared_v = list->v;
    vk->prepared_idx = list->idx;
    vk->prepared_cmd = list->cmd;
    vk->prepared_nv = list->nv;
    vk->prepared_ni = list->ni;
    vk->prepared_ncmd = list->ncmd;
    vk->prepared_slot = frame_slot;
    return true;
}

bool nk_ui_vk_render(NkUiVk *vk, const NkUiDrawList *list,
                     struct VkCommandBuffer_T *command_buffer,
                     uint32_t width, uint32_t height) {
    const NkUiVkFrame *frame;
    VkViewport viewport;
    VkDescriptorSet bound = VK_NULL_HANDLE;
    VkRect2D scissor;
    VkDeviceSize offset = 0;
    float size[2];
    bool have_scissor = false;
    uint32_t draws = 0;
    size_t i;
    if (vk == NULL || list == NULL || command_buffer == NULL || width == 0 || height == 0 ||
        width > (uint32_t)INT32_MAX || height > (uint32_t)INT32_MAX) {
        return false;
    }
    if (!vk->prepared || list != vk->prepared_list || list->v != vk->prepared_v ||
        list->idx != vk->prepared_idx || list->cmd != vk->prepared_cmd ||
        list->nv != vk->prepared_nv || list->ni != vk->prepared_ni ||
        list->ncmd != vk->prepared_ncmd) {
        return false;
    }
    if (vk->draw_count == 0) {
        return true;
    }
    frame = &vk->frames[vk->prepared_slot];
    vk->fn.CmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, vk->pipeline);
    memset(&viewport, 0, sizeof(viewport));
    viewport.width = (float)width;
    viewport.height = (float)height;
    viewport.maxDepth = 1.0f;
    vk->fn.CmdSetViewport(command_buffer, 0, 1, &viewport);
    size[0] = (float)width;
    size[1] = (float)height;
    vk->fn.CmdPushConstants(command_buffer, vk->pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT, 0,
                            (uint32_t)sizeof(size), size);
    vk->fn.CmdBindVertexBuffers(command_buffer, 0, 1, &frame->vertices.buffer, &offset);
    vk->fn.CmdBindIndexBuffer(command_buffer, frame->indices.buffer, 0, VK_INDEX_TYPE_UINT32);
    memset(&scissor, 0, sizeof(scissor));
    for (i = 0; i < vk->draw_count; ++i) {
        const NkUiVkDraw *draw = &vk->draws[i];
        /* The command's clip, clamped to the framebuffer; an empty one draws
           nothing (vkCmdSetScissor takes no negative offset). */
        int x0 = draw->clip[0] < 0 ? 0 : draw->clip[0];
        int y0 = draw->clip[1] < 0 ? 0 : draw->clip[1];
        int x1 = draw->clip[2] > (int)width ? (int)width : draw->clip[2];
        int y1 = draw->clip[3] > (int)height ? (int)height : draw->clip[3];
        if (x1 <= x0 || y1 <= y0) {
            continue;
        }
        if (!have_scissor || scissor.offset.x != x0 || scissor.offset.y != y0 ||
            scissor.extent.width != (uint32_t)(x1 - x0) ||
            scissor.extent.height != (uint32_t)(y1 - y0)) {
            scissor.offset.x = x0;
            scissor.offset.y = y0;
            scissor.extent.width = (uint32_t)(x1 - x0);
            scissor.extent.height = (uint32_t)(y1 - y0);
            vk->fn.CmdSetScissor(command_buffer, 0, 1, &scissor);
            have_scissor = true;
        }
        if (draw->set != bound) {
            vk->fn.CmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                         vk->pipeline_layout, 0, 1, &draw->set, 0, NULL);
            bound = draw->set;
        }
        vk->fn.CmdDrawIndexed(command_buffer, draw->count, 1, draw->first, 0, 0);
        ++draws;
    }
    vk->stats.draws_recorded += draws;
    return true;
}

/* ---- create and destroy ---- */

static bool nk_ui_vk_load(NkUiVk *vk, const NkUiVkInit *init) {
    PFN_vkGetInstanceProcAddr get = init->get_instance_proc_addr;
    vk->fn.GetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)get(init->instance, "vkGetDeviceProcAddr");
    vk->fn.GetPhysicalDeviceProperties = (PFN_vkGetPhysicalDeviceProperties)get(
        init->instance, "vkGetPhysicalDeviceProperties");
    vk->fn.GetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)get(
        init->instance, "vkGetPhysicalDeviceMemoryProperties");
    vk->fn.GetPhysicalDeviceQueueFamilyProperties = (PFN_vkGetPhysicalDeviceQueueFamilyProperties)get(
        init->instance, "vkGetPhysicalDeviceQueueFamilyProperties");
    if (vk->fn.GetDeviceProcAddr == NULL || vk->fn.GetPhysicalDeviceProperties == NULL ||
        vk->fn.GetPhysicalDeviceMemoryProperties == NULL ||
        vk->fn.GetPhysicalDeviceQueueFamilyProperties == NULL) {
        return false;
    }
#define NK_UI_VK_LOAD(name) \
    vk->fn.name = (PFN_vk##name)vk->fn.GetDeviceProcAddr(init->device, "vk" #name); \
    if (vk->fn.name == NULL) { \
        return false; \
    }
    NK_UI_VK_DEVICE_FUNCS(NK_UI_VK_LOAD)
#undef NK_UI_VK_LOAD
    return true;
}

static bool nk_ui_vk_device_ok(NkUiVk *vk, const NkUiVkInit *init) {
    VkPhysicalDeviceProperties properties;
    VkQueueFamilyProperties *families;
    uint32_t count = 0;
    bool graphics;
    vk->fn.GetPhysicalDeviceProperties(init->physical_device, &properties);
    if (properties.apiVersion < VK_API_VERSION_1_1) {
        return false; /* the embedded SPIR-V is 1.3 */
    }
    vk->page_size = NK_UI_VK_PAGE_SIZE;
    if (properties.limits.maxImageDimension2D < (uint32_t)vk->page_size) {
        vk->page_size = (int)properties.limits.maxImageDimension2D;
    }
    vk->image_max_side = NK_UI_VK_IMAGE_MAX_SIDE;
    if (properties.limits.maxImageDimension2D < (uint32_t)vk->image_max_side) {
        vk->image_max_side = (int)properties.limits.maxImageDimension2D;
    }
    vk->fn.GetPhysicalDeviceMemoryProperties(init->physical_device, &vk->memory);
    vk->fn.GetPhysicalDeviceQueueFamilyProperties(init->physical_device, &count, NULL);
    if (init->queue_family >= count) {
        return false;
    }
    families = (VkQueueFamilyProperties *)calloc(count, sizeof(*families));
    if (families == NULL) {
        return false;
    }
    vk->fn.GetPhysicalDeviceQueueFamilyProperties(init->physical_device, &count, families);
    graphics = init->queue_family < count &&
               (families[init->queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u;
    free(families);
    return graphics;
}

static bool nk_ui_vk_create_objects(NkUiVk *vk) {
    VkSamplerCreateInfo sampler;
    VkDescriptorSetLayoutBinding binding;
    VkDescriptorSetLayoutCreateInfo set_layout;
    VkPushConstantRange push;
    VkPipelineLayoutCreateInfo layout;
    memset(&sampler, 0, sizeof(sampler));
    sampler.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler.magFilter = VK_FILTER_NEAREST;
    sampler.minFilter = VK_FILTER_NEAREST;
    sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sampler.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler.maxLod = 0.0f;
    if (vk->fn.CreateSampler(vk->device, &sampler, vk->allocator, &vk->nearest) != VK_SUCCESS) {
        vk->nearest = VK_NULL_HANDLE;
        return false;
    }
    sampler.magFilter = VK_FILTER_LINEAR;
    sampler.minFilter = VK_FILTER_LINEAR;
    if (vk->fn.CreateSampler(vk->device, &sampler, vk->allocator, &vk->linear) != VK_SUCCESS) {
        vk->linear = VK_NULL_HANDLE;
        return false;
    }
    memset(&binding, 0, sizeof(binding));
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    memset(&set_layout, 0, sizeof(set_layout));
    set_layout.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    set_layout.bindingCount = 1;
    set_layout.pBindings = &binding;
    if (vk->fn.CreateDescriptorSetLayout(vk->device, &set_layout, vk->allocator,
                                         &vk->set_layout) != VK_SUCCESS) {
        vk->set_layout = VK_NULL_HANDLE;
        return false;
    }
    /* ui.vert: push_constant { vec2 output_size_px; } */
    push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push.offset = 0;
    push.size = (uint32_t)(sizeof(float) * 2u);
    memset(&layout, 0, sizeof(layout));
    layout.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layout.setLayoutCount = 1;
    layout.pSetLayouts = &vk->set_layout;
    layout.pushConstantRangeCount = 1;
    layout.pPushConstantRanges = &push;
    if (vk->fn.CreatePipelineLayout(vk->device, &layout, vk->allocator,
                                    &vk->pipeline_layout) != VK_SUCCESS) {
        vk->pipeline_layout = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

static bool nk_ui_vk_create_pipeline(NkUiVk *vk, const NkUiVkInit *init) {
    VkShaderModuleCreateInfo module_info;
    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2];
    VkVertexInputBindingDescription binding;
    VkVertexInputAttributeDescription attributes[3];
    VkPipelineVertexInputStateCreateInfo vertex_input;
    VkPipelineInputAssemblyStateCreateInfo assembly;
    VkPipelineViewportStateCreateInfo viewport;
    VkPipelineRasterizationStateCreateInfo raster;
    VkPipelineMultisampleStateCreateInfo multisample;
    VkPipelineDepthStencilStateCreateInfo depth;
    VkPipelineColorBlendAttachmentState attachment;
    VkPipelineColorBlendStateCreateInfo blend;
    static const VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                                     VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic;
    VkGraphicsPipelineCreateInfo info;
    VkResult result;
    memset(&module_info, 0, sizeof(module_info));
    module_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    module_info.codeSize = sizeof(k_nk_ui_vert_spv);
    module_info.pCode = k_nk_ui_vert_spv;
    if (vk->fn.CreateShaderModule(vk->device, &module_info, vk->allocator, &vert) != VK_SUCCESS) {
        return false;
    }
    module_info.codeSize = sizeof(k_nk_ui_frag_spv);
    module_info.pCode = k_nk_ui_frag_spv;
    if (vk->fn.CreateShaderModule(vk->device, &module_info, vk->allocator, &frag) != VK_SUCCESS) {
        vk->fn.DestroyShaderModule(vk->device, vert, vk->allocator);
        return false;
    }
    memset(stages, 0, sizeof(stages));
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vert;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = frag;
    stages[1].pName = "main";
    binding.binding = 0;
    binding.stride = (uint32_t)sizeof(NkUiVkVertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    attributes[0].location = 0;
    attributes[0].binding = 0;
    attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[0].offset = (uint32_t)offsetof(NkUiVkVertex, x);
    attributes[1].location = 1;
    attributes[1].binding = 0;
    attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributes[1].offset = (uint32_t)offsetof(NkUiVkVertex, u);
    attributes[2].location = 2;
    attributes[2].binding = 0;
    attributes[2].format = VK_FORMAT_R8G8B8A8_UNORM;
    attributes[2].offset = (uint32_t)offsetof(NkUiVkVertex, rgba);
    memset(&vertex_input, 0, sizeof(vertex_input));
    vertex_input.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertex_input.vertexBindingDescriptionCount = 1;
    vertex_input.pVertexBindingDescriptions = &binding;
    vertex_input.vertexAttributeDescriptionCount = 3;
    vertex_input.pVertexAttributeDescriptions = attributes;
    memset(&assembly, 0, sizeof(assembly));
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    memset(&viewport, 0, sizeof(viewport));
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    memset(&raster, 0, sizeof(raster));
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;
    memset(&multisample, 0, sizeof(multisample));
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = init->samples != 0 ? init->samples : VK_SAMPLE_COUNT_1_BIT;
    memset(&depth, 0, sizeof(depth));
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    memset(&attachment, 0, sizeof(attachment));
    attachment.blendEnable = VK_TRUE;
    attachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.colorBlendOp = VK_BLEND_OP_ADD;
    attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    attachment.alphaBlendOp = VK_BLEND_OP_ADD;
    attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    memset(&blend, 0, sizeof(blend));
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &attachment;
    memset(&dynamic, 0, sizeof(dynamic));
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamic_states;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertex_input;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &raster;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = vk->pipeline_layout;
    info.renderPass = init->render_pass;
    info.subpass = init->subpass;
    result = vk->fn.CreateGraphicsPipelines(vk->device, init->pipeline_cache, 1, &info,
                                            vk->allocator, &vk->pipeline);
    vk->fn.DestroyShaderModule(vk->device, vert, vk->allocator);
    vk->fn.DestroyShaderModule(vk->device, frag, vk->allocator);
    if (result != VK_SUCCESS) {
        vk->pipeline = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

NkUiVk *nk_ui_vk_create(const NkUiVkInit *init) {
    NkUiVk *vk;
    if (init == NULL || init->get_instance_proc_addr == NULL || init->instance == VK_NULL_HANDLE ||
        init->physical_device == VK_NULL_HANDLE || init->device == VK_NULL_HANDLE ||
        init->queue == VK_NULL_HANDLE || init->render_pass == VK_NULL_HANDLE ||
        init->frames_in_flight == 0 || init->frames_in_flight > NK_UI_VK_MAX_FRAMES) {
        return NULL;
    }
    vk = (NkUiVk *)calloc(1, sizeof(*vk));
    if (vk == NULL) {
        return NULL;
    }
    vk->instance = init->instance;
    vk->physical_device = init->physical_device;
    vk->device = init->device;
    vk->queue = init->queue;
    vk->allocator = init->allocator;
    vk->frames_in_flight = init->frames_in_flight;
    vk->retire_slot = -1;
    vk->density = 0.0f;
    if (init->text != NULL) {
        vk->text = *init->text;
    }
    if (!nk_ui_vk_load(vk, init)) {
        free(vk);
        return NULL;
    }
    if (!nk_ui_vk_device_ok(vk, init) || !nk_ui_vk_create_objects(vk) ||
        !nk_ui_vk_create_pipeline(vk, init)) {
        nk_ui_vk_destroy(vk);
        return NULL;
    }
    vk->white = nk_ui_vk_texture_create(vk, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false, vk->nearest);
    if (vk->white == NULL) {
        nk_ui_vk_destroy(vk);
        return NULL;
    }
    return vk;
}

void nk_ui_vk_destroy(NkUiVk *vk) {
    size_t i;
    if (vk == NULL) {
        return;
    }
    vk->fn.QueueWaitIdle(vk->queue);
    for (i = 0; i < NK_UI_VK_MAX_FRAMES; ++i) {
        nk_ui_vk_collect(vk, &vk->frames[i]);
        nk_ui_vk_buffer_destroy(vk, &vk->frames[i].vertices);
        nk_ui_vk_buffer_destroy(vk, &vk->frames[i].indices);
        nk_ui_vk_buffer_destroy(vk, &vk->frames[i].staging);
    }
    vk->retire_slot = -1;
    nk_ui_vk_drop_atlas(vk);
    for (i = 0; i < NK_UI_VK_IMAGE_CACHE; ++i) {
        nk_ui_vk_texture_destroy(vk, vk->images[i].texture);
    }
    for (i = 0; i < NK_UI_VK_WAVES_SLOTS; ++i) {
        nk_ui_vk_texture_destroy(vk, vk->waves[i].texture);
        free(vk->waves[i].pixels);
    }
    nk_ui_vk_texture_destroy(vk, vk->white);
    if (vk->pipeline != VK_NULL_HANDLE) {
        vk->fn.DestroyPipeline(vk->device, vk->pipeline, vk->allocator);
    }
    if (vk->pipeline_layout != VK_NULL_HANDLE) {
        vk->fn.DestroyPipelineLayout(vk->device, vk->pipeline_layout, vk->allocator);
    }
    if (vk->set_layout != VK_NULL_HANDLE) {
        vk->fn.DestroyDescriptorSetLayout(vk->device, vk->set_layout, vk->allocator);
    }
    for (i = 0; i < vk->pool_count; ++i) {
        vk->fn.DestroyDescriptorPool(vk->device, vk->pools[i], vk->allocator);
    }
    if (vk->linear != VK_NULL_HANDLE) {
        vk->fn.DestroySampler(vk->device, vk->linear, vk->allocator);
    }
    if (vk->nearest != VK_NULL_HANDLE) {
        vk->fn.DestroySampler(vk->device, vk->nearest, vk->allocator);
    }
    free(vk->pools);
    free(vk->entries);
    free(vk->table);
    free(vk->glyphs);
    free(vk->extra_vertices);
    free(vk->extra_indices);
    free(vk->draws);
    free(vk->upload);
    free(vk->touched);
    free(vk->barriers);
    free(vk->copies);
    free(vk->regions);
    free(vk);
}

void nk_ui_vk_set_scale(NkUiVk *vk, float density) {
    if (vk == NULL || !(density > 0.0f) || !isfinite(density) || density == vk->density) {
        return;
    }
    if (vk->page_count > 0 || vk->entry_count > 0) {
        ++vk->stats.atlas_rebuilds;
    }
    nk_ui_vk_drop_atlas(vk);
    vk->density = density;
}

void nk_ui_vk_get_stats(const NkUiVk *vk, NkUiVkStats *out) {
    if (out == NULL) {
        return;
    }
    if (vk == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = vk->stats;
    out->atlas_pages = (uint32_t)vk->page_count;
    out->atlas_glyphs = (uint32_t)vk->entry_count;
}

#endif /* NK_UI_WITH_VULKAN */
