/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_smooth_selftest.c - Ace Mode's in-between frames on a headless Vulkan device. Links the pass
 * and the C library alone (invariant S1). Pictures are source-owned shapes on a noise floor.
 *
 *   between-moves   a shape 16 px further on in the new frame lands 8 px on in the in-between
 *   hud-exact       a corner that does not change keeps every byte, while the scene moves
 *   cut-holds       two unrelated pictures: the in-between is the old one, byte for byte
 *   held-is-new     the frame ace_smooth_held returns is the new frame, byte for byte
 *   source-unread   the pass never writes the source image, and its result is another image
 *   no-predecessor  the first frame gives no in-between
 *   scales          the same scene at 2x render scale
 *   hud-list        the HUD box rules: small 2D boxes are HUD; a quarter screen, a box that samples
 *                   a render target, or too many boxes are not; a present hands its boxes on once
 *   hud-edge        the scene right beside a HUD box is right with the mask (it is wrong without:
 *                   one end of every vector there is under the HUD), and the HUD keeps every byte
 *   depth-band      a band that matches the scene behind it in the old frame: only depth tells the
 *                   band's vector from the scene's at its edge; the depth image is only read
 *   aux-neutral     an empty mask gives the same bytes as no mask
 *
 * Exit 77 (SKIP) without a Vulkan loader or device.
 */
#include "ace_smooth.h"
#include "ace_hud.h"

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
        if (!(cond)) { s_fail = 1; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                       printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

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
    VkBuffer xfer;
    VkDeviceMemory xfer_memory;
    uint32_t *map;
} Gpu;
static Gpu g;

typedef struct { VkImage image; VkDeviceMemory memory; } Source;

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
GPU_FN(CmdPipelineBarrier); GPU_FN(CmdCopyImageToBuffer); GPU_FN(CmdCopyBufferToImage);
GPU_FN(QueueSubmit); GPU_FN(DeviceWaitIdle);
#undef GPU_FN

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    GetPhysicalDeviceMemoryProperties(g.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

#define XFER_BYTES (1024u * 544u * 4u)

static int gpu_open(char *why, size_t cap) {
#if defined(_WIN32)
    HMODULE lib = LoadLibraryA("vulkan-1.dll");
    if (lib) g.gipa = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(lib, "vkGetInstanceProcAddr");
#else
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (lib) *(void **)&g.gipa = dlsym(lib, "vkGetInstanceProcAddr");
#endif
    if (!g.gipa) { snprintf(why, cap, "no Vulkan loader"); return 0; }
    CreateInstance = (PFN_vkCreateInstance)g.gipa(VK_NULL_HANDLE, "vkCreateInstance");
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "ace_smooth_selftest", 1, NULL, 0,
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
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT)) {
                g.physical = devs[d];
                g.family = q;
                break;
            }
    }
    if (!g.physical) { snprintf(why, cap, "no graphics and compute queue"); return 0; }
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
    DEV(CmdCopyImageToBuffer); DEV(CmdCopyBufferToImage); DEV(QueueSubmit); DEV(DeviceWaitIdle);
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
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = XFER_BYTES;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (CreateBuffer(g.device, &bi, NULL, &g.xfer) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr;
    GetBufferMemoryRequirements(g.device, g.xfer, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &g.xfer_memory) != VK_SUCCESS ||
        BindBufferMemory(g.device, g.xfer, g.xfer_memory, 0) != VK_SUCCESS ||
        MapMemory(g.device, g.xfer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&g.map) != VK_SUCCESS) return -1;
    return 1;
}

static void gpu_close(void) {
    if (g.device) {
        DeviceWaitIdle(g.device);
        if (g.xfer) DestroyBuffer(g.device, g.xfer, NULL);
        if (g.xfer_memory) FreeMemory(g.device, g.xfer_memory, NULL);
        if (g.fence) DestroyFence(g.device, g.fence, NULL);
        if (g.pool) DestroyCommandPool(g.device, g.pool, NULL);
        DestroyDevice(g.device, NULL);
    }
    if (g.instance) DestroyInstance(g.instance, NULL);
}

static void cmd_begin(void) {
    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    ResetCommandBuffer(g.cb, 0);
    BeginCommandBuffer(g.cb, &bi);
}

static void cmd_end(void) {
    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cb;
    EndCommandBuffer(g.cb);
    ResetFences(g.device, 1, &g.fence);
    QueueSubmit(g.queue, 1, &si, g.fence);
    WaitForFences(g.device, 1, &g.fence, VK_TRUE, UINT64_MAX);
}

static void barrier(VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    CmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 0, NULL, 0, NULL, 1, &b);
}

/* A source image holding g.map's picture, left in TRANSFER_SRC_OPTIMAL. */
static int source_make(Source *src, int w, int h) {
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (CreateImage(g.device, &ii, NULL, &src->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    GetImageMemoryRequirements(g.device, src->image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &src->memory) != VK_SUCCESS ||
        BindImageMemory(g.device, src->image, src->memory, 0) != VK_SUCCESS) return 0;
    return 1;
}

static void source_fill(const Source *src, int w, int h, int fresh) {
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w;
    c.imageExtent.height = (uint32_t)h;
    c.imageExtent.depth = 1;
    cmd_begin();
    barrier(src->image, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CmdCopyBufferToImage(g.cb, g.xfer, src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    barrier(src->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cmd_end();
}

/* Reads an image in TRANSFER_SRC_OPTIMAL into g.map. */
static void image_read(VkImage image, int w, int h) {
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w;
    c.imageExtent.height = (uint32_t)h;
    c.imageExtent.depth = 1;
    cmd_begin();
    CmdCopyImageToBuffer(g.cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.xfer, 1, &c);
    cmd_end();
}

/* ---- pictures --------------------------------------------------------------------------------- */

static uint32_t noise(int x, int y) {
    uint32_t h = (uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    return h;
}

/* A floor of smooth bumps with a little grain, so a block always has something to match. */
static uint32_t floor_px(int x, int y, int seed) {
    uint32_t n = noise((x / 4 + seed) & 1023, y / 4);
    uint32_t v = 60u + (n & 63u) + (noise(x, y) & 3u);
    return 0xFF000000u | (v << 16) | (v << 8) | v;
}

#define SHAPE_W 48
#define SHAPE_H 40
#define SHAPE_Y 100
#define HUD_X 8
#define HUD_Y 8
#define HUD_W 64
#define HUD_H 16
#define HUD_PX 0xFF20E0F0u
#define SHAPE_PX 0xFF2060FFu

/* A scene that pans `pan` px and a shape at shape_x, under a HUD that never moves. */
static void paint(int w, int h, int scale, int pan, int shape_x, int hud) {
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int gx = x / scale, gy = y / scale;
            uint32_t v = floor_px(gx + pan, gy, 0);
            if (gx >= shape_x && gx < shape_x + SHAPE_W && gy >= SHAPE_Y && gy < SHAPE_Y + SHAPE_H) {
                v = SHAPE_PX;
                if (((gx - shape_x) / 8 + (gy - SHAPE_Y) / 8) & 1) v ^= 0x00404040u;
            }
            if (hud && gx >= HUD_X && gx < HUD_X + HUD_W && gy >= HUD_Y && gy < HUD_Y + HUD_H)
                v = HUD_PX;
            g.map[y * w + x] = v;
        }
    }
}

/* Another picture altogether, in blobs a search at any level sees. */
static void paint_other(int w, int h, int scale) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            g.map[y * w + x] = 0xFF000000u | noise(x / scale / 8 + 5000, y / scale / 8);
}

/* The mean x of the pixels close to the shape's colour, in guest pixels. */
static double shape_centre(int w, int h, int scale, int *count) {
    double sum = 0.0;
    int n = 0;
    for (int y = SHAPE_Y * scale; y < (SHAPE_Y + SHAPE_H) * scale && y < h; y++) {
        for (int x = 0; x < w; x++) {
            uint32_t v = g.map[y * w + x];
            int r = (int)(v & 255u), b = (int)((v >> 16) & 255u);
            if (r > 170 && b < 120) { sum += (double)x / scale; n++; }
        }
    }
    *count = n;
    return n ? sum / n : -1.0;
}

static AceBetween *smooth_new(void) {
    AceSmoothInit init;
    char error[160] = "";
    memset(&init, 0, sizeof(init));
    init.get_instance_proc_addr = g.gipa;
    init.instance = g.instance;
    init.physical_device = g.physical;
    init.device = g.device;
    init.queue = g.queue;
    init.queue_family = g.family;
    AceBetween *s = ace_smooth_create(&init, error, sizeof(error));
    CHECK(s != NULL, "create failed: %s", error);
    return s;
}

/* Feeds one frame; returns the in-between when asked and one exists. */
static VkImage feed(AceBetween *s, const Source *src, int w, int h, int between) {
    cmd_begin();
    VkImage out = ace_smooth_record(s, g.cb, src->image, w, h, between);
    cmd_end();
    return out;
}

static void cell_scene(int scale) {
    int w = 480 * scale, h = 272 * scale;
    uint32_t *a = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
    uint32_t *b = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
    Source src;
    memset(&src, 0, sizeof(src));
    AceBetween *s = smooth_new();
    if (!a || !b || !s || !source_make(&src, w, h)) {
        CHECK(0, "cell setup failed");
        free(a); free(b); ace_smooth_destroy(s);
        return;
    }
    const int shape_a = 200, step = 16;

    paint(w, h, scale, 0, shape_a, 1);
    memcpy(a, g.map, (size_t)w * (size_t)h * 4u);
    source_fill(&src, w, h, 1);
    CHECK(feed(s, &src, w, h, 1) == VK_NULL_HANDLE, "no-predecessor: an in-between from one frame");

    paint(w, h, scale, 0, shape_a + step, 1);
    memcpy(b, g.map, (size_t)w * (size_t)h * 4u);
    source_fill(&src, w, h, 0);
    VkImage mid = feed(s, &src, w, h, 1);
    CHECK(mid != VK_NULL_HANDLE, "no in-between");
    CHECK(mid != src.image, "source-unread: the in-between is the source");
    if (mid) {
        image_read(mid, w, h);
        int n = 0;
        double centre = shape_centre(w, h, scale, &n);
        double want = shape_a + step / 2 + SHAPE_W / 2.0;
        printf("  scale %d: shape centre %.2f, want %.2f, %d px\n", scale, centre, want, n);
        CHECK(centre > want - 1.0 && centre < want + 1.0, "between-moves: centre %.2f, want %.2f", centre, want);
        /* It is not a blend of the two: a blend leaves half-strength shapes at both places. */
        int ghost = 0;
        for (int y = (SHAPE_Y + 8) * scale; y < (SHAPE_Y + 32) * scale; y++) {
            for (int x = (shape_a + 2) * scale; x < (shape_a + 6) * scale; x++) {
                uint32_t v = g.map[y * w + x];
                int r = (int)(v & 255u);
                if (v != a[y * w + x] && r > 130) ghost++;
            }
        }
        CHECK(ghost * 10 < 4 * scale * 24 * scale, "between-moves: %d ghost px at the old place", ghost);
        int bad = 0;
        for (int y = HUD_Y * scale; y < (HUD_Y + HUD_H) * scale; y++)
            for (int x = HUD_X * scale; x < (HUD_X + HUD_W) * scale; x++)
                if (g.map[y * w + x] != HUD_PX) bad++;
        CHECK(bad == 0, "hud-exact: %d HUD px changed", bad);
    }
    cmd_begin();
    VkImage held = ace_smooth_held(s, g.cb);
    cmd_end();
    CHECK(held != VK_NULL_HANDLE, "held-is-new: no held frame");
    if (held) {
        image_read(held, w, h);
        CHECK(memcmp(g.map, b, (size_t)w * (size_t)h * 4u) == 0, "held-is-new: bytes differ");
    }
    image_read(src.image, w, h);
    CHECK(memcmp(g.map, b, (size_t)w * (size_t)h * 4u) == 0, "source-unread: the source changed");

    /* A cut: the in-between is the old frame, the one before the cut. */
    paint_other(w, h, scale);
    source_fill(&src, w, h, 0);
    mid = feed(s, &src, w, h, 1);
    CHECK(mid != VK_NULL_HANDLE, "cut: no in-between");
    if (mid) {
        image_read(mid, w, h);
        size_t differ = 0;
        for (size_t i = 0; i < (size_t)w * (size_t)h; i++) differ += g.map[i] != b[i];
        CHECK(differ == 0, "cut-holds: %zu px differ from the old frame", differ);
    }
    DeviceWaitIdle(g.device);
    ace_smooth_destroy(s);
    DestroyImage(g.device, src.image, NULL);
    FreeMemory(g.device, src.memory, NULL);
    free(a);
    free(b);
}

/* A whole-picture pan: every block moves together, a fast camera. */
static void cell_pan(void) {
    int w = 480, h = 272;
    Source src;
    memset(&src, 0, sizeof(src));
    AceBetween *s = smooth_new();
    if (!s || !source_make(&src, w, h)) { CHECK(0, "pan setup failed"); ace_smooth_destroy(s); return; }
    paint(w, h, 1, 0, 5000, 0);
    source_fill(&src, w, h, 1);
    (void)feed(s, &src, w, h, 0);
    paint(w, h, 1, 40, 5000, 0);
    source_fill(&src, w, h, 0);
    VkImage mid = feed(s, &src, w, h, 1);
    CHECK(mid != VK_NULL_HANDLE, "pan: no in-between");
    if (mid) {
        image_read(mid, w, h);
        /* The true in-between, away from the edges where one frame has no data. */
        int wrong = 0, total = 0;
        for (int y = 16; y < h - 16; y++) {
            for (int x = 48; x < w - 48; x++) {
                int want = (int)(floor_px(x + 20, y, 0) & 255u);
                int got = (int)(g.map[y * w + x] & 255u);
                if (abs(want - got) > 12) wrong++;
                total++;
            }
        }
        printf("  pan: %d of %d px wrong\n", wrong, total);
        CHECK(wrong * 20 < total, "pan: %d of %d px wrong", wrong, total);
    }
    DeviceWaitIdle(g.device);
    ace_smooth_destroy(s);
    DestroyImage(g.device, src.image, NULL);
    FreeMemory(g.device, src.memory, NULL);
}

/* ---- the HUD mask and depth (ace_hud.h, AceSmoothAux) ----------------------------------------- */

#define BAND_X 200
#define BAND_W 64
#define BAND_STEP 16
#define DEPTH_NEAR 16384u   /* 0.25 */
#define DEPTH_FAR 49152u    /* 0.75 */

typedef struct { VkImage image; VkDeviceMemory memory; int w, h; } Depth;

static void depth_barrier(VkImage image, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    CmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                       0, 0, NULL, 0, NULL, 1, &b);
}

/* A D16 image like the GE's: wider than the presented frame, with the frame at its top-left. */
static int depth_make(Depth *d, int w, int h) {
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_D16_UNORM;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
               VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if (CreateImage(g.device, &ii, NULL, &d->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    GetImageMemoryRequirements(g.device, d->image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &d->memory) != VK_SUCCESS ||
        BindImageMemory(g.device, d->image, d->memory, 0) != VK_SUCCESS) return 0;
    d->w = w;
    d->h = h;
    return 1;
}

static void depth_free(Depth *d) {
    if (d->image) DestroyImage(g.device, d->image, NULL);
    if (d->memory) FreeMemory(g.device, d->memory, NULL);
    memset(d, 0, sizeof(*d));
}

/* g.map, read as d->w x d->h 16-bit values, into the image, which is left the way the GE leaves
 * a depth buffer: DEPTH_STENCIL_ATTACHMENT_OPTIMAL. */
static void depth_fill(const Depth *d, int fresh) {
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)d->w;
    c.imageExtent.height = (uint32_t)d->h;
    c.imageExtent.depth = 1;
    cmd_begin();
    depth_barrier(d->image, fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CmdCopyBufferToImage(g.cb, g.xfer, d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    depth_barrier(d->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    cmd_end();
}

static void depth_read(const Depth *d) {
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)d->w;
    c.imageExtent.height = (uint32_t)d->h;
    c.imageExtent.depth = 1;
    cmd_begin();
    depth_barrier(d->image, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    CmdCopyImageToBuffer(g.cb, d->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g.xfer, 1, &c);
    depth_barrier(d->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    cmd_end();
}

static VkImage feed_aux(AceBetween *s, const Source *src, int w, int h, int between, const AceSmoothAux *aux) {
    cmd_begin();
    VkImage out = ace_smooth_record_aux(s, g.cb, src->image, w, h, between, aux);
    cmd_end();
    return out;
}

static int luma_off(uint32_t got, uint32_t want) {
    return abs((int)(got & 255u) - (int)(want & 255u)) > 12;
}

/* The list rules, on the CPU: what is HUD and what is not. */
static void cell_hud_list(void) {
    static AceHudList l;
    static uint32_t bits[ACE_SMOOTH_HUD_WORDS];
    ace_hud_list_reset(&l);
    CHECK(ace_hud_list_add(&l, 20.2f, 30.0f, 10.5f, 34.0f, 0) == 1, "hud-list: a small box is not kept");
    CHECK(ace_hud_list_add(&l, 10.5f, 30.0f, 20.2f, 34.0f, 0) == 1 && l.count == 1,
          "hud-list: the second triangle of a quad added a box (%u)", l.count);
    CHECK(ace_hud_list_add(&l, 0.0f, 0.0f, 240.0f, 136.0f, 0) == 0, "hud-list: a quarter-screen box is kept");
    CHECK(ace_hud_list_add(&l, 0.0f, 0.0f, 480.0f, 272.0f, 0) == 0, "hud-list: a full-screen box is kept");
    CHECK(ace_hud_list_add(&l, 50.0f, 50.0f, 60.0f, 60.0f, 1) == 0, "hud-list: a box sampling a target is kept");
    CHECK(ace_hud_list_add(&l, -40.0f, 260.0f, 4.0f, 300.0f, 0) == 1, "hud-list: a box off the edge is not clipped");
    CHECK(ace_hud_list_bits(&l, bits) == 1, "hud-list: no mask");
    int set = 0, wrong = 0;
    for (int y = 0; y < ACE_SMOOTH_HUD_H; y++) {
        for (int x = 0; x < ACE_SMOOTH_HUD_W; x++) {
            uint32_t k = (uint32_t)(y * ACE_SMOOTH_HUD_W + x);
            int bit = (int)((bits[k >> 5] >> (k & 31u)) & 1u);
            int want = (x >= 10 && x < 21 && y >= 30 && y < 34) || (x < 4 && y >= 260);
            set += bit;
            wrong += bit != want;
        }
    }
    CHECK(wrong == 0 && set == 11 * 4 + 4 * 12, "hud-list: %d bits wrong, %d set", wrong, set);
    ace_hud_list_reset(&l);
    for (uint32_t i = 0; i <= ACE_HUD_MAX_BOXES; i++)
        (void)ace_hud_list_add(&l, (float)(i % 400), (float)(i / 400), (float)(i % 400) + 1.0f,
                               (float)(i / 400) + 1.0f, 0);
    CHECK(l.overflow == 1, "hud-list: %u boxes did not overflow", ACE_HUD_MAX_BOXES + 1u);
    CHECK(ace_hud_list_bits(&l, bits) == 0 && bits[0] == 0u, "hud-list: an overflowed list gave a mask");

    /* The runtime side: a present hands over its target's boxes once, and depth only when the
     * target's depth moved since its last present. */
    ace_hud_enable(1);
    CHECK(g_ace_hud_on == 1, "hud-runtime: not on");
    ace_hud_note(3, 100.0f, 100.0f, 110.0f, 104.0f, 0);
    ace_hud_note(2, 0.0f, 0.0f, 8.0f, 8.0f, 0);
    VkImage fake = (VkImage)(uintptr_t)0x1234u;
    ace_hud_present(3, fake, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 512, 272, 7u);
    const AceSmoothAux *aux = ace_hud_take();
    CHECK(aux && aux->hud_bits && aux->depth == fake, "hud-runtime: the first present gave no mask or depth");
    if (aux && aux->hud_bits) {
        uint32_t k = 102u * ACE_SMOOTH_HUD_W + 105u, k2 = 4u * ACE_SMOOTH_HUD_W + 4u;
        CHECK((aux->hud_bits[k >> 5] >> (k & 31u)) & 1u, "hud-runtime: slot 3's box is not in its mask");
        CHECK(!((aux->hud_bits[k2 >> 5] >> (k2 & 31u)) & 1u), "hud-runtime: slot 2's box is in slot 3's mask");
    }
    CHECK(ace_hud_take() == NULL, "hud-runtime: a present was taken twice");
    ace_hud_present(3, fake, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 512, 272, 7u);
    aux = ace_hud_take();
    CHECK(aux && aux->depth == VK_NULL_HANDLE, "hud-runtime: depth no batch wrote was handed on");
    if (aux && aux->hud_bits) {
        uint32_t k = 102u * ACE_SMOOTH_HUD_W + 105u;
        CHECK(!((aux->hud_bits[k >> 5] >> (k & 31u)) & 1u), "hud-runtime: last frame's box is still in the mask");
    }
    ace_hud_enable(0);
    ace_hud_note(3, 100.0f, 100.0f, 110.0f, 104.0f, 0);
    ace_hud_present(3, fake, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 512, 272, 8u);
    CHECK(ace_hud_take() == NULL, "hud-runtime: off, but a present was published");
}

#define EDGE_HUD_X 200
#define EDGE_HUD_Y 112
#define EDGE_HUD_W 64
#define EDGE_HUD_H 40
#define EDGE_PAN 16

static void paint_edge(int w, int h, int scale, int pan) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int gx = x / scale, gy = y / scale;
            uint32_t v = floor_px(gx + pan, gy, 0);
            if (gx >= EDGE_HUD_X && gx < EDGE_HUD_X + EDGE_HUD_W && gy >= EDGE_HUD_Y && gy < EDGE_HUD_Y + EDGE_HUD_H)
                v = HUD_PX;
            g.map[y * w + x] = v;
        }
}

static void edge_bits(uint32_t *bits) {
    memset(bits, 0, ACE_SMOOTH_HUD_WORDS * 4u);
    for (int y = EDGE_HUD_Y; y < EDGE_HUD_Y + EDGE_HUD_H; y++)
        for (int x = EDGE_HUD_X; x < EDGE_HUD_X + EDGE_HUD_W; x++) {
            uint32_t k = (uint32_t)(y * ACE_SMOOTH_HUD_W + x);
            bits[k >> 5] |= 1u << (k & 31u);
        }
}

/* Wrong px beside the HUD (in its rows, within a pan of its sides), and changed HUD px; by_col
 * counts the wrong px in each of the 2 * EDGE_PAN columns, left strip first. control counts the
 * wrong px in as many columns of the same rows far from the HUD: what the search gets wrong in
 * this scene anyway. */
#define EDGE_CONTROL_A 40
#define EDGE_CONTROL_B 400
static void edge_score(int w, int scale, int *wrong, int *total, int *hud_bad, int *by_col, int *control) {
    *wrong = *total = *hud_bad = *control = 0;
    memset(by_col, 0, 2 * EDGE_PAN * sizeof(int));
    for (int gy = EDGE_HUD_Y; gy < EDGE_HUD_Y + EDGE_HUD_H; gy++) {
        for (int i = 0; i < 2 * EDGE_PAN; i++) {
            int gx = i < EDGE_PAN ? EDGE_CONTROL_A + i : EDGE_CONTROL_B + i - EDGE_PAN;
            *control += luma_off(g.map[(gy * scale) * w + gx * scale], floor_px(gx + EDGE_PAN / 2, gy, 0));
        }
        for (int gx = EDGE_HUD_X - EDGE_PAN; gx < EDGE_HUD_X + EDGE_HUD_W + EDGE_PAN; gx++) {
            uint32_t got = g.map[(gy * scale) * w + gx * scale];
            if (gx >= EDGE_HUD_X && gx < EDGE_HUD_X + EDGE_HUD_W) {
                *hud_bad += got != HUD_PX;
                continue;
            }
            int bad = luma_off(got, floor_px(gx + EDGE_PAN / 2, gy, 0));
            *wrong += bad;
            by_col[gx < EDGE_HUD_X ? gx - (EDGE_HUD_X - EDGE_PAN) : gx - (EDGE_HUD_X + EDGE_HUD_W) + EDGE_PAN] += bad;
            (*total)++;
        }
    }
}

/* A HUD box over a panning scene. The scene right beside the HUD is found in only one frame at
 * any vector (the other end is under the HUD), so without the mask it keeps the old frame's
 * pixels; with the mask it takes the frame whose end is clear: under 5% wrong, no worse than the
 * same rows far from the HUD (the search's own misses in this scene), and less than half as wrong
 * as without it. */
static void cell_hud_edge(int scale) {
    int w = 480 * scale, h = 272 * scale;
    static uint32_t bits[ACE_SMOOTH_HUD_WORDS];
    edge_bits(bits);
    AceSmoothAux aux;
    memset(&aux, 0, sizeof(aux));
    aux.hud_bits = bits;
    Source src;
    memset(&src, 0, sizeof(src));
    if (!source_make(&src, w, h)) { CHECK(0, "hud-edge setup failed"); return; }
    int wrong[2] = { 0, 0 }, total = 0, hud_bad[2] = { 0, 0 }, by_col[2][2 * EDGE_PAN] = { { 0 } };
    int control[2] = { 0, 0 };
    for (int use = 0; use < 2; use++) {
        AceBetween *s = smooth_new();
        if (!s) break;
        paint_edge(w, h, scale, 0);
        source_fill(&src, w, h, use == 0);
        (void)feed_aux(s, &src, w, h, 0, use ? &aux : NULL);
        paint_edge(w, h, scale, EDGE_PAN);
        source_fill(&src, w, h, 0);
        VkImage mid = feed_aux(s, &src, w, h, 1, use ? &aux : NULL);
        CHECK(mid != VK_NULL_HANDLE, "hud-edge: no in-between");
        if (mid) {
            image_read(mid, w, h);
            edge_score(w, scale, &wrong[use], &total, &hud_bad[use], by_col[use], &control[use]);
        }
        DeviceWaitIdle(g.device);
        ace_smooth_destroy(s);
    }
    printf("  hud-edge scale %d: beside the HUD %d of %d px wrong without the mask, %d with it;"
           " far from it %d and %d\n", scale, wrong[0], total, wrong[1], control[0], control[1]);
    if (wrong[1] * 20 >= total || wrong[1] > control[1] || wrong[1] * 2 >= wrong[0]) {
        /* Where it went wrong: wrong px per column, from EDGE_PAN left of the HUD to EDGE_PAN right. */
        for (int use = 0; use < 2; use++) {
            printf("    %s:", use ? "with" : "without");
            for (int i = 0; i < 2 * EDGE_PAN; i++) printf("%s %d", i == EDGE_PAN ? " |" : "", by_col[use][i]);
            printf("\n");
        }
    }
    CHECK(hud_bad[0] == 0 && hud_bad[1] == 0, "hud-edge: HUD px changed (%d without, %d with)", hud_bad[0],
          hud_bad[1]);
    CHECK(wrong[1] * 20 < total, "hud-edge: %d of %d px beside the HUD wrong with the mask", wrong[1], total);
    CHECK(wrong[1] <= control[1], "hud-edge: %d of %d px beside the HUD wrong with the mask, %d far from it",
          wrong[1], total, control[1]);
    CHECK(wrong[1] * 2 < wrong[0], "hud-edge: the mask did not help (%d -> %d)", wrong[0], wrong[1]);
    DestroyImage(g.device, src.image, NULL);
    FreeMemory(g.device, src.memory, NULL);
}

/* A camouflaged band: in the old frame it shows exactly the scene behind it, so the old frame
 * alone cannot tell it from the scene. Then the band moves right while the scene pans left. For
 * band pixels near its left edge, the scene's vector fits the colours as well as the band's own,
 * and only depth says which one joins one surface: the band is near, the scene far. */
static void paint_band(int w, int h, int scale, int half_steps, uint16_t *depth, int dw) {
    /* half_steps: 0 the old frame, 2 the new one, 1 the true in-between */
    int bx = BAND_X + half_steps * BAND_STEP / 2;
    int pan = half_steps * BAND_STEP / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int gx = x / scale, gy = y / scale;
            int in_band = gx >= bx && gx < bx + BAND_W;
            uint32_t v = in_band ? floor_px(gx - pan, gy, 0)   /* the band carries the old scene */
                                 : floor_px(gx + pan, gy, 0);
            g.map[y * w + x] = v;
            if (depth) depth[y * dw + x] = (uint16_t)(in_band ? DEPTH_NEAR : DEPTH_FAR);
        }
}

static void cell_depth_band(int scale) {
    int w = 480 * scale, h = 272 * scale;
    int dw = 512 * scale;
    Source src;
    Depth dep;
    memset(&src, 0, sizeof(src));
    memset(&dep, 0, sizeof(dep));
    uint32_t *truth = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
    uint16_t *zbuf = (uint16_t *)malloc((size_t)dw * (size_t)h * 2u);
    if (!truth || !zbuf || !source_make(&src, w, h) || !depth_make(&dep, dw, h)) {
        CHECK(0, "depth-band setup failed");
        free(truth); free(zbuf);
        return;
    }
    paint_band(w, h, scale, 1, NULL, 0);
    memcpy(truth, g.map, (size_t)w * (size_t)h * 4u);
    int wrong[2] = { 0, 0 }, total = 0;
    for (int use = 0; use < 2; use++) {
        AceBetween *s = smooth_new();
        if (!s) break;
        AceSmoothAux aux;
        memset(&aux, 0, sizeof(aux));
        aux.depth = dep.image;
        aux.depth_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        aux.depth_w = dw;
        aux.depth_h = h;
        for (int f = 0; f < 2; f++) {
            paint_band(w, h, scale, 2 * f, zbuf, dw);
            source_fill(&src, w, h, use == 0 && f == 0);
            memcpy(g.map, zbuf, (size_t)dw * (size_t)h * 2u);
            depth_fill(&dep, use == 0 && f == 0);
            VkImage mid = feed_aux(s, &src, w, h, f, use ? &aux : NULL);
            if (f == 0) continue;
            CHECK(mid != VK_NULL_HANDLE, "depth-band: no in-between");
            if (!mid) break;
            image_read(mid, w, h);
            /* The band's pixels near its edges in the true in-between, where both frames see them. */
            int bx = BAND_X + BAND_STEP / 2, n = 0, bad = 0;
            for (int gy = 40; gy < 232; gy++)
                for (int gx = bx; gx < bx + BAND_W; gx++) {
                    if (gx >= bx + BAND_STEP && gx < bx + BAND_W - BAND_STEP) continue;
                    size_t i = (size_t)(gy * scale) * (size_t)w + (size_t)(gx * scale);
                    bad += luma_off(g.map[i], truth[i]);
                    n++;
                }
            wrong[use] = bad;
            total = n;
        }
        if (use) {
            /* The depth image is only read, and is left as it came. */
            depth_read(&dep);
            CHECK(memcmp(g.map, zbuf, (size_t)dw * (size_t)h * 2u) == 0, "depth-band: the depth image changed");
        }
        DeviceWaitIdle(g.device);
        ace_smooth_destroy(s);
    }
    printf("  depth-band scale %d: band edge %d of %d px wrong without depth, %d with it\n", scale, wrong[0],
           total, wrong[1]);
    CHECK(wrong[1] * 10 < total, "depth-band: %d of %d band px wrong with depth", wrong[1], total);
    CHECK(wrong[1] * 2 < wrong[0], "depth-band: depth did not help (%d -> %d)", wrong[0], wrong[1]);
    depth_free(&dep);
    DestroyImage(g.device, src.image, NULL);
    FreeMemory(g.device, src.memory, NULL);
    free(truth);
    free(zbuf);
}

/* An empty mask and no depth change nothing: the in-between is byte for byte the one without. */
static void cell_aux_neutral(void) {
    int w = 480, h = 272;
    static uint32_t zero[ACE_SMOOTH_HUD_WORDS];
    AceSmoothAux aux;
    memset(&aux, 0, sizeof(aux));
    aux.hud_bits = zero;
    Source src;
    memset(&src, 0, sizeof(src));
    uint32_t *first = (uint32_t *)malloc((size_t)w * (size_t)h * 4u);
    if (!first || !source_make(&src, w, h)) { CHECK(0, "aux-neutral setup failed"); free(first); return; }
    for (int use = 0; use < 2; use++) {
        AceBetween *s = smooth_new();
        if (!s) break;
        paint(w, h, 1, 0, 200, 1);
        source_fill(&src, w, h, use == 0);
        (void)feed_aux(s, &src, w, h, 0, use ? &aux : NULL);
        paint(w, h, 1, 12, 216, 1);
        source_fill(&src, w, h, 0);
        VkImage mid = feed_aux(s, &src, w, h, 1, use ? &aux : NULL);
        CHECK(mid != VK_NULL_HANDLE, "aux-neutral: no in-between");
        if (mid) {
            image_read(mid, w, h);
            if (!use) memcpy(first, g.map, (size_t)w * (size_t)h * 4u);
            else CHECK(memcmp(first, g.map, (size_t)w * (size_t)h * 4u) == 0,
                       "aux-neutral: an empty mask changed the in-between");
        }
        DeviceWaitIdle(g.device);
        ace_smooth_destroy(s);
    }
    DestroyImage(g.device, src.image, NULL);
    FreeMemory(g.device, src.memory, NULL);
    free(first);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    char why[128] = "";
    int up = gpu_open(why, sizeof(why));
    if (up <= 0) {
        printf("SKIP ace-smooth-selftest: %s\n", why[0] ? why : "the device could not be set up");
        gpu_close();
        return up == 0 ? 77 : 1;
    }
    cell_scene(1);
    cell_scene(2);
    cell_pan();
    cell_hud_list();
    cell_hud_edge(1);
    cell_hud_edge(2);
    cell_depth_band(1);
    cell_depth_band(2);
    cell_aux_neutral();
    gpu_close();
    printf(s_fail ? "ace-smooth-selftest: FAIL\n" : "ace-smooth-selftest: PASS\n");
    return s_fail ? 1 : 0;
}
