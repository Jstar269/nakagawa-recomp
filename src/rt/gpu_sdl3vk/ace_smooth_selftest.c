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
 *
 * Exit 77 (SKIP) without a Vulkan loader or device.
 */
#include "ace_smooth.h"

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
    gpu_close();
    printf(s_fail ? "ace-smooth-selftest: FAIL\n" : "ace-smooth-selftest: PASS\n");
    return s_fail ? 1 : 0;
}
