/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_ui_selftest.c - Ace Mode's in-game UI (ace_ui.c), with synthetic frames only.
 *
 *   layout  CPU only: the readout, the launch notice and the keyboard at the overlay's window
 *           sizes and two ultrawide windows: every element inside the window and its 16 px safe
 *           area, the two chips apart, the keyboard under them; an empty frame draws nothing.
 *   gpu     on a headless Vulkan device: the pass loads an image cleared to one colour, draws
 *           the chips and the keyboard over it, and leaves every other pixel as it was. The
 *           image is written to <dir>/ace_ui_selftest.ppm for review. Exit 77 without Vulkan.
 *
 * Linked from ace_ui.c, osk_overlay.c, nk_ui and its Vulkan backend alone: a reference from the
 * UI pass to anything of the runtime fails the link (invariant S1). With --fonts <player
 * folder> and SDL3_ttf the GPU frame uses the launcher's fonts instead of block glyphs.
 */
#define VK_NO_PROTOTYPES 1
#include "ace_ui.h"

#include "nk_ui_draw.h"
#include "nk_ui_theme.h"
#if defined(NK_UI_WITH_SDL_TTF)
#include "backend/nk_ui_sdlr.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

static int s_fail;

#define CHECK(cond, ...) do { \
        if (!(cond)) { s_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
                       fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } \
    } while (0)

/* ---- a block-glyph text backend: 8 px advance, 16 px line ---------------------------------- */

static uint8_t s_glyph[16 * 8];

static NkUiTextSize text_measure(void *user, int font, int px, const char *s, size_t n) {
    NkUiTextSize size;
    (void)user; (void)font; (void)s;
    size.width = (float)n * (float)px * 0.55f;
    size.height = (float)px * 1.25f;
    size.baseline = (float)px;
    return size;
}

static int text_shape(void *user, int font, int px, const char *s, size_t n, NkUiGlyph *out, int max) {
    int i;
    (void)user; (void)font;
    for (i = 0; i < max && (size_t)i < n; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].glyph = (uint8_t)s[i];
        out[i].codepoint = (uint8_t)s[i];
        out[i].x = (float)i * (float)px * 0.55f;
        out[i].advance_x = (float)px * 0.55f;
    }
    return i;
}

static bool text_raster(void *user, int font, int px, uint32_t glyph, NkUiGlyphBitmap *out) {
    (void)user; (void)font; (void)px;
    memset(out, 0, sizeof(*out));
    if (glyph == ' ') return false;
    memset(s_glyph, 0xFF, sizeof(s_glyph));
    out->pixels = s_glyph;
    out->width = 6;
    out->height = 10;
    out->pitch = 8;
    out->bearing_y = 10;
    out->advance_x = 7.0f;
    return true;
}

static const NkUiTextBackend k_blocks = {NULL, text_measure, text_shape, text_raster};

/* ---- frames ---------------------------------------------------------------------------------- */

static OskOverlay s_keyboard;

static void keyboard_open(void) {
    static const uint16_t desc[] = {'Y','o','u','r',' ','n','a','m','e',0};
    static const uint16_t initial[] = {'A','c','e',0};
    osk_overlay_reset(&s_keyboard);
    osk_overlay_open(&s_keyboard, desc, initial, 16, 0u);
}

static AceUiFrame frame_full(int w, int h, float scale) {
    AceUiFrame f;
    memset(&f, 0, sizeof(f));
    f.pixel_w = w;
    f.pixel_h = h;
    f.display_scale = scale;
    f.now_s = 1.0;
    f.reduce_motion = 1;
    f.notice = "16x filtering \xC2\xB7 crisp keyboard \xC2\xB7 speed readout";
    f.notice_alpha = 1.0f;
    f.readout = "\xE2\x96\xB2 87%  26 FPS  PC LIMITED";
    f.readout_warning = 1;
    f.keyboard = &s_keyboard;
    return f;
}

/* The union of the bounds of every command an element drew, in device px; 0 when it drew none. */
static int element_bounds(const NkUiDrawList *list, NkUiId id, int out[4]) {
    int found = 0;
    for (uint32_t i = 0; i < list->ncmd; i++) {
        const NkUiDrawCmd *c = &list->cmd[i];
        if (c->element_id != id || c->kind == NK_UI_DRAW_SCISSOR_START || c->kind == NK_UI_DRAW_SCISSOR_END)
            continue;
        if (!found) { memcpy(out, c->bounds, sizeof(int) * 4u); found = 1; continue; }
        if (c->bounds[0] < out[0]) out[0] = c->bounds[0];
        if (c->bounds[1] < out[1]) out[1] = c->bounds[1];
        if (c->bounds[2] > out[2]) out[2] = c->bounds[2];
        if (c->bounds[3] > out[3]) out[3] = c->bounds[3];
    }
    return found;
}

static const NkUiDrawList *build(NkUi *ui, const AceUiFrame *f, float *density) {
    NkUiViewport vp;
    ace_ui_viewport(f, &vp);
    *density = nk_ui_scale_for_viewport(&vp).density;
    nk_ui_set_viewport(ui, &vp);
    nk_ui_begin(ui, f->now_s);
    ace_ui_layout(ui, f);
    return nk_ui_end(ui);
}

/* ---- layout ---------------------------------------------------------------------------------- */

static void cell_layout(void) {
    static const struct { int w, h; float scale; } sizes[] = {
        /* the overlay's three, a 1x and a 2x game window, 720p, and two ultrawide windows */
        {960, 600, 1.0f}, {1280, 800, 1.0f}, {1920, 1200, 1.5f}, {480, 272, 1.0f},
        {960, 544, 1.0f}, {1280, 720, 1.0f}, {2560, 1080, 1.0f}, {5120, 1440, 2.0f},
    };
    NkUiConfig config;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = &k_blocks;
    NkUi *ui = nk_ui_create(&config);
    CHECK(ui != NULL, "nk_ui_create");
    if (!ui) return;
    keyboard_open();
    for (size_t s = 0; s < sizeof(sizes) / sizeof(sizes[0]); s++) {
        AceUiFrame f = frame_full(sizes[s].w, sizes[s].h, sizes[s].scale);
        float density = 1.0f;
        const NkUiDrawList *list = build(ui, &f, &density);
        CHECK(list != NULL, "%dx%d: the frame was dropped", f.pixel_w, f.pixel_h);
        if (!list) continue;
        int readout[4], notice[4], keys[4];
        int safe = (int)(16.0f * density) - 1;
        CHECK(element_bounds(list, ACE_UI_ID_READOUT, readout), "%dx%d: no readout", f.pixel_w, f.pixel_h);
        CHECK(element_bounds(list, ACE_UI_ID_NOTICE, notice), "%dx%d: no notice", f.pixel_w, f.pixel_h);
        CHECK(element_bounds(list, ACE_UI_ID_KEYBOARD, keys), "%dx%d: no keyboard", f.pixel_w, f.pixel_h);
        const int *rects[3] = { readout, notice, keys };
        for (int r = 0; r < 3; r++) {
            CHECK(rects[r][0] >= safe && rects[r][1] >= safe && rects[r][2] <= f.pixel_w - safe &&
                  rects[r][3] <= f.pixel_h - safe,
                  "%dx%d: element %d (%d,%d)-(%d,%d) leaves the safe area", f.pixel_w, f.pixel_h, r,
                  rects[r][0], rects[r][1], rects[r][2], rects[r][3]);
        }
        CHECK(readout[2] < notice[0], "%dx%d: the chips overlap", f.pixel_w, f.pixel_h);
        CHECK(readout[0] < f.pixel_w / 2 && notice[2] > f.pixel_w / 2,
              "%dx%d: readout top left, notice top right", f.pixel_w, f.pixel_h);
        CHECK(keys[1] >= readout[3] && keys[1] >= notice[3], "%dx%d: the keyboard covers a chip",
              f.pixel_w, f.pixel_h);
        /* No element draws outside the window. */
        for (uint32_t i = 0; i < list->ncmd; i++) {
            const NkUiDrawCmd *c = &list->cmd[i];
            if (c->kind == NK_UI_DRAW_SCISSOR_START || c->kind == NK_UI_DRAW_SCISSOR_END) continue;
            CHECK(c->bounds[0] >= 0 && c->bounds[1] >= 0 && c->bounds[2] <= f.pixel_w &&
                  c->bounds[3] <= f.pixel_h, "%dx%d: command %u outside the window", f.pixel_w,
                  f.pixel_h, i);
        }
    }
    /* Nothing to show: no frame is wanted, and a hidden notice draws nothing. */
    AceUiFrame empty;
    memset(&empty, 0, sizeof(empty));
    empty.pixel_w = 960;
    empty.pixel_h = 544;
    empty.display_scale = 1.0f;
    CHECK(!ace_ui_frame_wants(&empty), "an empty frame is wanted");
    empty.notice = "16x filtering";
    empty.notice_alpha = 0.0f;
    CHECK(!ace_ui_frame_wants(&empty), "a faded-out notice is wanted");
    empty.notice_alpha = 0.5f;
    CHECK(ace_ui_frame_wants(&empty), "a notice half faded in is not wanted");
    osk_overlay_reset(&s_keyboard);
    AceUiFrame closed = frame_full(960, 544, 1.0f);
    closed.readout = NULL;
    closed.notice = NULL;
    CHECK(!ace_ui_frame_wants(&closed), "a closed keyboard is wanted");
    nk_ui_destroy(ui);
}

/* ---- gpu ------------------------------------------------------------------------------------- */

typedef struct {
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkImage image;
    VkDeviceMemory image_memory;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    void *pixels;
} Gpu;

static Gpu g;

#define GPU_FN(name) static PFN_vk##name name
GPU_FN(CreateInstance); GPU_FN(DestroyInstance); GPU_FN(EnumeratePhysicalDevices);
GPU_FN(GetPhysicalDeviceQueueFamilyProperties); GPU_FN(GetPhysicalDeviceMemoryProperties);
GPU_FN(CreateDevice); GPU_FN(DestroyDevice); GPU_FN(GetDeviceProcAddr); GPU_FN(GetDeviceQueue);
GPU_FN(CreateCommandPool); GPU_FN(DestroyCommandPool); GPU_FN(AllocateCommandBuffers);
GPU_FN(CreateFence); GPU_FN(DestroyFence); GPU_FN(WaitForFences); GPU_FN(ResetFences);
GPU_FN(CreateImage); GPU_FN(DestroyImage); GPU_FN(GetImageMemoryRequirements);
GPU_FN(CreateBuffer); GPU_FN(DestroyBuffer); GPU_FN(GetBufferMemoryRequirements);
GPU_FN(AllocateMemory); GPU_FN(FreeMemory); GPU_FN(BindImageMemory); GPU_FN(BindBufferMemory);
GPU_FN(MapMemory); GPU_FN(BeginCommandBuffer); GPU_FN(EndCommandBuffer); GPU_FN(ResetCommandBuffer);
GPU_FN(CmdPipelineBarrier); GPU_FN(CmdClearColorImage); GPU_FN(CmdCopyImageToBuffer);
GPU_FN(QueueSubmit); GPU_FN(DeviceWaitIdle);
#undef GPU_FN

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    GetPhysicalDeviceMemoryProperties(g.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int gpu_open(char *why, size_t cap, int w, int h) {
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (lib) g.gipa = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib) *(void **)&g.gipa = dlsym(lib, "vkGetInstanceProcAddr");
#endif
    if (!g.gipa) { snprintf(why, cap, "no Vulkan loader"); return 0; }
    CreateInstance = (PFN_vkCreateInstance)g.gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ace_ui_selftest", 1, NULL, 0,
                              VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (!CreateInstance || CreateInstance(&ici, NULL, &g.instance) != VK_SUCCESS) {
        snprintf(why, cap, "no usable Vulkan driver");
        return 0;
    }
#define INST(name) name = (PFN_vk##name)g.gipa(g.instance, "vk" #name)
    INST(DestroyInstance); INST(EnumeratePhysicalDevices); INST(GetPhysicalDeviceQueueFamilyProperties);
    INST(GetPhysicalDeviceMemoryProperties); INST(CreateDevice); INST(GetDeviceProcAddr);
#undef INST
    VkPhysicalDevice devs[8];
    uint32_t nd = 8;
    if (EnumeratePhysicalDevices(g.instance, &nd, devs) < VK_SUCCESS || nd == 0) {
        snprintf(why, cap, "no Vulkan device");
        return 0;
    }
    for (uint32_t d = 0; d < nd && !g.physical; d++) {
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16;
        GetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
        for (uint32_t q = 0; q < nq; q++)
            if (qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) { g.physical = devs[d]; g.family = q; break; }
    }
    if (!g.physical) { snprintf(why, cap, "no graphics queue"); return 0; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (CreateDevice(g.physical, &dci, NULL, &g.device) != VK_SUCCESS) {
        snprintf(why, cap, "vkCreateDevice failed");
        return 0;
    }
#define DEV(name) name = (PFN_vk##name)GetDeviceProcAddr(g.device, "vk" #name)
    DEV(DestroyDevice); DEV(GetDeviceQueue); DEV(CreateCommandPool); DEV(DestroyCommandPool);
    DEV(AllocateCommandBuffers); DEV(CreateFence); DEV(DestroyFence); DEV(WaitForFences);
    DEV(ResetFences); DEV(CreateImage); DEV(DestroyImage); DEV(GetImageMemoryRequirements);
    DEV(CreateBuffer); DEV(DestroyBuffer); DEV(GetBufferMemoryRequirements); DEV(AllocateMemory);
    DEV(FreeMemory); DEV(BindImageMemory); DEV(BindBufferMemory); DEV(MapMemory);
    DEV(BeginCommandBuffer); DEV(EndCommandBuffer); DEV(ResetCommandBuffer); DEV(CmdPipelineBarrier);
    DEV(CmdClearColorImage); DEV(CmdCopyImageToBuffer); DEV(QueueSubmit); DEV(DeviceWaitIdle);
#undef DEV
    GetDeviceQueue(g.device, g.family, 0, &g.queue);
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g.family };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL,
                                        VK_NULL_HANDLE, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    if (CreateCommandPool(g.device, &pci, NULL, &g.pool) != VK_SUCCESS) return -1;
    cai.commandPool = g.pool;
    if (AllocateCommandBuffers(g.device, &cai, &g.cb) != VK_SUCCESS ||
        CreateFence(g.device, &fci, NULL, &g.fence) != VK_SUCCESS) return -1;
    /* The stand-in swapchain image: the formats and usage the presenter's images have. */
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_B8G8R8A8_UNORM;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    VkMemoryRequirements mr;
    if (CreateImage(g.device, &ii, NULL, &g.image) != VK_SUCCESS) return -1;
    GetImageMemoryRequirements(g.device, g.image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &g.image_memory) != VK_SUCCESS ||
        BindImageMemory(g.device, g.image, g.image_memory, 0) != VK_SUCCESS) return -1;
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = (VkDeviceSize)w * (VkDeviceSize)h * 4u;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (CreateBuffer(g.device, &bi, NULL, &g.readback) != VK_SUCCESS) return -1;
    GetBufferMemoryRequirements(g.device, g.readback, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &g.readback_memory) != VK_SUCCESS ||
        BindBufferMemory(g.device, g.readback, g.readback_memory, 0) != VK_SUCCESS ||
        MapMemory(g.device, g.readback_memory, 0, VK_WHOLE_SIZE, 0, &g.pixels) != VK_SUCCESS) return -1;
    return 1;
}

static void gpu_close(void) {
    if (g.device) {
        DeviceWaitIdle(g.device);
        if (g.readback) DestroyBuffer(g.device, g.readback, NULL);
        if (g.readback_memory) FreeMemory(g.device, g.readback_memory, NULL);
        if (g.image) DestroyImage(g.device, g.image, NULL);
        if (g.image_memory) FreeMemory(g.device, g.image_memory, NULL);
        if (g.fence) DestroyFence(g.device, g.fence, NULL);
        if (g.pool) DestroyCommandPool(g.device, g.pool, NULL);
        DestroyDevice(g.device, NULL);
    }
    if (g.instance) DestroyInstance(g.instance, NULL);
}

static void barrier(VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                    VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = g.image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    CmdPipelineBarrier(g.cb, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

/* One present: clear to the "game frame" colour, the pass, read back. Returns the pass's result. */
static int present(AceUi *ui, const AceUiFrame *f, int w, int h, uint32_t slot) {
    VkCommandBufferBeginInfo bbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                     VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    ResetCommandBuffer(g.cb, 0);
    BeginCommandBuffer(g.cb, &bbi);
    barrier(VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue frame_colour = {{0.25f, 0.5f, 0.75f, 1.0f}};
    VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    CmdClearColorImage(g.cb, g.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &frame_colour, 1, &rng);
    VkImage images[1] = { g.image };
    AceUiTarget t;
    memset(&t, 0, sizeof(t));
    t.cmd = g.cb;
    t.frame_slot = slot;
    t.format = VK_FORMAT_B8G8R8A8_UNORM;
    t.extent.width = (uint32_t)w;
    t.extent.height = (uint32_t)h;
    t.images = images;
    t.image_count = 1;
    t.image_index = 0;
    t.swapchain_gen = 1;
    int drew = ace_ui_record(ui, &t, f);
    barrier(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w;
    c.imageExtent.height = (uint32_t)h;
    c.imageExtent.depth = 1;
    CmdCopyImageToBuffer(g.cb, g.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.readback, 1, &c);
    EndCommandBuffer(g.cb);
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cb;
    ResetFences(g.device, 1, &g.fence);
    if (QueueSubmit(g.queue, 1, &si, g.fence) != VK_SUCCESS) return -1;
    WaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
    return drew;
}

static uint32_t pixel(int w, int x, int y) {
    const uint8_t *p = (const uint8_t *)g.pixels + ((size_t)y * (size_t)w + (size_t)x) * 4u;
    return ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];   /* BGRA -> 0xRRGGBB */
}

static void write_ppm(const char *path, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t c = pixel(w, x, y);
            unsigned char rgb[3] = { (unsigned char)(c >> 16), (unsigned char)(c >> 8), (unsigned char)c };
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

#if defined(NK_UI_WITH_SDL_TTF)
/* The launcher's fonts from a player folder, as the overlay host registers them. */
static NkUiTtf *fonts_open(const char *root) {
    NkUiTtf *ttf = nk_ui_ttf_create(NULL, 0);
    if (!ttf) return NULL;
    for (int font = 0; font < NK_UI_FONT_COUNT; font++) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/%s", root, nk_ui_font_path(font));
        if (!nk_ui_ttf_add_font(ttf, font, path)) { nk_ui_ttf_destroy(ttf); return NULL; }
    }
    for (int font = 0; font < NK_UI_FONT_COUNT; font++) {
        if (font == NK_UI_FONT_CJK) continue;
        if (font != NK_UI_FONT_BODY) (void)nk_ui_ttf_set_fallback(ttf, font, NK_UI_FONT_BODY);
        (void)nk_ui_ttf_set_fallback(ttf, font, NK_UI_FONT_CJK);
    }
    return ttf;
}
#endif

static int cell_gpu(const char *dir, const char *fonts_root) {
    const int w = 1280, h = 720;
    char why[128] = "";
    int opened = gpu_open(why, sizeof(why), w, h);
    if (opened <= 0) {
        gpu_close();
        if (opened == 0) {
            printf("ace-ui-selftest: gpu SKIP (%s)\n", why);
            return 77;
        }
        CHECK(0, "the headless device could not be set up");
        return 1;
    }
    const NkUiTextBackend *text = &k_blocks;
#if defined(NK_UI_WITH_SDL_TTF)
    NkUiTtf *ttf = NULL;
    NkUiTextBackend ttf_text;
    if (fonts_root) {
        ttf = fonts_open(fonts_root);
        if (ttf) { ttf_text = nk_ui_ttf_backend(ttf); text = &ttf_text; }
        else printf("ace-ui-selftest: the fonts under %s could not be loaded; block glyphs\n", fonts_root);
    }
#else
    (void)fonts_root;
#endif
    AceUiInit init;
    memset(&init, 0, sizeof(init));
    init.get_instance_proc_addr = g.gipa;
    init.instance = g.instance;
    init.physical_device = g.physical;
    init.device = g.device;
    init.queue = g.queue;
    init.queue_family = g.family;
    init.frames_in_flight = 1;
    init.text = text;
    char error[256];
    AceUi *ui = ace_ui_create(&init, error, sizeof(error));
    CHECK(ui != NULL, "ace_ui_create: %s", error);
    if (ui) {
        AceUiFrame empty;
        memset(&empty, 0, sizeof(empty));
        empty.pixel_w = w;
        empty.pixel_h = h;
        empty.display_scale = 1.0f;
        CHECK(present(ui, &empty, w, h, 0) == 0, "an empty frame drew");
        /* The clear colour as this device stores it, from a frame the pass left alone. */
        const uint32_t frame_colour = pixel(w, w / 2, h / 2);
        CHECK(pixel(w, 22, 22) == frame_colour && pixel(w, w - 22, 22) == frame_colour &&
              pixel(w, w / 2, h - 60) == frame_colour, "an empty frame changed the image");
        keyboard_open();
        AceUiFrame f = frame_full(w, h, 1.0f);
        CHECK(present(ui, &f, w, h, 0) == 1, "the full frame did not draw");
        /* The game frame shows between the chips and above the keyboard. */
        CHECK(pixel(w, w / 2, 40) == frame_colour, "the frame between the chips changed: %06x",
              pixel(w, w / 2, 40));
        CHECK(pixel(w, w / 2, h / 4) == frame_colour, "the frame above the keyboard changed: %06x",
              pixel(w, w / 2, h / 4));
        /* Each chip and the keyboard covered the frame where it stands. */
        CHECK(pixel(w, 22, 22) != frame_colour, "no readout chip at the top left");
        CHECK(pixel(w, w - 22, 22) != frame_colour, "no notice chip at the top right");
        CHECK(pixel(w, w / 2, h - 60) != frame_colour, "no keyboard at the foot of the window");
        /* The 16 px safe area stays the game's. */
        CHECK(pixel(w, 8, 8) == frame_colour && pixel(w, w - 8, h - 8) == frame_colour,
              "the safe area changed");
        char path[600];
        snprintf(path, sizeof(path), "%s%sace_ui_selftest.ppm", dir ? dir : "", dir ? "/" : "");
        write_ppm(path, w, h);
        printf("ace-ui-selftest: gpu frame written to %s\n", path);
        ace_ui_destroy(ui);
    }
#if defined(NK_UI_WITH_SDL_TTF)
    nk_ui_ttf_destroy(ttf);
#endif
    gpu_close();
    return 0;
}

int main(int argc, char **argv) {
    const char *dir = NULL, *fonts = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--fonts") == 0 && i + 1 < argc) fonts = argv[++i];
        else dir = argv[i];
    }
    cell_layout();
    int gpu = cell_gpu(dir, fonts);
    if (s_fail) {
        fprintf(stderr, "ace-ui-selftest: %d check(s) FAILED\n", s_fail);
        return 1;
    }
    if (gpu == 77) {
        /* The layout passed; the GPU leg did not run. 77 keeps that visible. */
        printf("ace-ui-selftest: layout PASS; gpu SKIP\n");
        return 77;
    }
    printf("ace-ui-selftest: PASS (layout, gpu)\n");
    return 0;
}
