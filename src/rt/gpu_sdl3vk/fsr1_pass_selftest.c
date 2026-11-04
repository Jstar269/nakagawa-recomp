/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr1_pass_selftest.c - the presentation upscaler (fsr1_pass.c, AMD FSR 1 EASU + RCAS) on a
 * headless Vulkan device. Links the pass, the C library and libm alone. Pictures are source-owned.
 *
 *   edge-vertical   EASU alone at 2x keeps a hard step sharper than the presenter's bilinear blit,
 *   edge-slanted    by a measured margin (mean error from the ideal step at most 2/3 of
 *                   bilinear's); RCAS keeps it at least as sharp
 *   flat-exact      a flat colour from a BGRA source stays that colour in every pixel at a
 *                   non-integer ratio (480x272 to 1355x768): no channel swap, no stray edge texels
 *   source-unread   the pass never writes the image it is given, and returns its own
 *   cost            GPU time of EASU alone and of EASU + RCAS from 1920x1088 (4x render scale) to
 *                   3840x2160, by timestamp queries, median of 15 (reported, not judged)
 *
 * --easu-spv FILE runs the edge cells with another EASU shader (a mutant); they must then fail.
 * Exit 77 (SKIP) without a Vulkan loader or device.
 */
#include "fsr1_pass.h"

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
GPU_FN(CmdBlitImage); GPU_FN(GetPhysicalDeviceProperties);
GPU_FN(CreateQueryPool); GPU_FN(DestroyQueryPool); GPU_FN(CmdResetQueryPool);
GPU_FN(CmdWriteTimestamp); GPU_FN(GetQueryPoolResults);
GPU_FN(QueueSubmit); GPU_FN(DeviceWaitIdle);
#undef GPU_FN

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    GetPhysicalDeviceMemoryProperties(g.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

#define XFER_BYTES (2048u * 1088u * 4u)

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
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "fsr1_selftest", 1, NULL, 0,
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
    INST(GetPhysicalDeviceProperties);
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
    DEV(CmdBlitImage); DEV(CreateQueryPool); DEV(DestroyQueryPool); DEV(CmdResetQueryPool);
    DEV(CmdWriteTimestamp); DEV(GetQueryPoolResults);
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

/* ---- images and the reference ------------------------------------------------------------------ */

static int image_make(Source *im, int w, int h, VkFormat format) {
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (CreateImage(g.device, &ii, NULL, &im->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    GetImageMemoryRequirements(g.device, im->image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &im->memory) != VK_SUCCESS ||
        BindImageMemory(g.device, im->image, im->memory, 0) != VK_SUCCESS) return 0;
    return 1;
}

static void image_free(Source *im) {
    DeviceWaitIdle(g.device);
    if (im->image) DestroyImage(g.device, im->image, NULL);
    if (im->memory) FreeMemory(g.device, im->memory, NULL);
    memset(im, 0, sizeof(*im));
}

/* Bilinear w x h -> ow x oh with a linear blit, the presenter's own non-integer filter, left in
 * TRANSFER_SRC_OPTIMAL and read into g.map. */
static void bilinear(const Source *src, int w, int h, int ow, int oh) {
    Source dst;
    memset(&dst, 0, sizeof(dst));
    if (!image_make(&dst, ow, oh, VK_FORMAT_R8G8B8A8_UNORM)) { CHECK(0, "reference image"); return; }
    VkImageBlit b;
    memset(&b, 0, sizeof(b));
    b.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.srcSubresource.layerCount = 1;
    b.dstSubresource = b.srcSubresource;
    b.srcOffsets[1].x = w; b.srcOffsets[1].y = h; b.srcOffsets[1].z = 1;
    b.dstOffsets[1].x = ow; b.dstOffsets[1].y = oh; b.dstOffsets[1].z = 1;
    cmd_begin();
    barrier(dst.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    CmdBlitImage(g.cb, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst.image,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_LINEAR);
    barrier(dst.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    cmd_end();
    image_read(dst.image, ow, oh);
    image_free(&dst);
}

static VkImage upscale(Fsr1Pass *p, const Source *src, int w, int h, int ow, int oh, float sharp) {
    cmd_begin();
    VkImage out = fsr1_record(p, g.cb, src->image, w, h, ow, oh, sharp);
    cmd_end();
    if (out) image_read(out, ow, oh);
    return out;
}

/* ---- pictures and measures ---------------------------------------------------------------------- */

#define LO 24u
#define HI 232u

static uint32_t grey(uint32_t v) { return v | v << 8 | v << 16 | 0xFF000000u; }

/* A hard step from LO to HI: vertical at x = w/2 + 0.5 px, or slanted (one column per three rows),
 * the edge a game's geometry and text have. */
static void paint_step(int w, int h, int slant) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int edge = w / 2 + (slant ? (y - h / 2) / 3 : 0);
            g.map[y * w + x] = grey(x < edge ? LO : HI);
        }
}

/* Mean over the middle rows of the pixels in a row that sit between 10% and 90% of the step: the
 * width of the edge in output pixels. */
static double edge_width(int w, int h) {
    double sum = 0;
    int rows = 0;
    for (int y = h / 4; y < h * 3 / 4; y++) {
        int n = 0;
        for (int x = 2; x < w - 2; x++) {
            uint32_t v = g.map[y * w + x] & 0xFFu;
            if (v > LO + (HI - LO) / 10u && v < HI - (HI - LO) / 10u) n++;
        }
        sum += n;
        rows++;
    }
    return rows ? sum / rows : 0;
}

/* The ideal output of the step at 2x: each source pixel twice, with no ramp. The mean absolute
 * difference from it over the middle rows. */
static double step_error(int w, int h, int slant) {
    double sum = 0;
    long n = 0;
    for (int y = h / 4; y < h * 3 / 4; y++)
        for (int x = 2; x < w - 2; x++) {
            int sy = y / 2, sx = x / 2;
            int edge = (w / 2) / 2 + (slant ? (sy - (h / 2) / 2) / 3 : 0);
            uint32_t want = sx < edge ? LO : HI;
            uint32_t v = g.map[y * w + x] & 0xFFu;
            sum += v > want ? v - want : want - v;
            n++;
        }
    return n ? sum / (double)n : 0;
}

/* ---- cells -------------------------------------------------------------------------------------- */

static int s_mutant;

/* edge-adaptive: EASU alone at 2x keeps a step edge sharper than the presenter's bilinear blit,
 * by a measured margin, for a vertical and a slanted edge. RCAS keeps it at least as sharp. */
static void cell_edge(Fsr1Pass *p, int slant) {
    const int w = 96, h = 96, ow = 192, oh = 192;
    Source src;
    memset(&src, 0, sizeof(src));
    if (!image_make(&src, w, h, VK_FORMAT_R8G8B8A8_UNORM)) { CHECK(0, "source image"); return; }
    paint_step(w, h, slant);
    source_fill(&src, w, h, 1);
    bilinear(&src, w, h, ow, oh);
    double bw = edge_width(ow, oh), be = step_error(ow, oh, slant);
    VkImage out = upscale(p, &src, w, h, ow, oh, FSR1_SHARPNESS_OFF);
    CHECK(out != VK_NULL_HANDLE, "EASU recorded");
    double ew = edge_width(ow, oh), ee = step_error(ow, oh, slant);
    upscale(p, &src, w, h, ow, oh, FSR1_SHARPNESS_DEFAULT);
    double rw = edge_width(ow, oh), re = step_error(ow, oh, slant);
    printf("fsr1-selftest: %s edge at 2x: ramp width bilinear %.2f px, EASU %.2f px, EASU+RCAS %.2f px; "
           "mean error from the ideal step bilinear %.2f, EASU %.2f, EASU+RCAS %.2f\n",
           slant ? "slanted" : "vertical", bw, ew, rw, be, ee, re);
    /* The margin: EASU's edge error is at most two thirds of bilinear's. */
    CHECK(ee <= be * 2.0 / 3.0, "EASU is not sharper than bilinear on the %s edge (error %.2f vs %.2f)%s",
          slant ? "slanted" : "vertical", ee, be, s_mutant ? " [the mutant is caught]" : "");
    CHECK(ew <= bw, "EASU's ramp (%.2f px) is wider than bilinear's (%.2f px)", ew, bw);
    CHECK(re <= ee + 0.5 && rw <= ew, "RCAS softened the edge (error %.2f vs %.2f)", re, ee);
    image_free(&src);
}

/* flat-exact: a flat colour stays that colour, every pixel, through both passes, at a non-integer
 * ratio (480x272 to 1355x768, a window's fit) and from a BGRA source: no channel swap, no stray
 * edge texels. */
static void cell_flat(Fsr1Pass *p) {
    const int w = 480, h = 272, ow = 1355, oh = 768;
    Source src;
    memset(&src, 0, sizeof(src));
    if (!image_make(&src, w, h, VK_FORMAT_B8G8R8A8_UNORM)) { CHECK(0, "source image"); return; }
    /* Memory order B, G, R, A: blue 0x30, green 0x80, red 0xC0. */
    for (int i = 0; i < w * h; i++) g.map[i] = 0xFFC08030u;
    source_fill(&src, w, h, 1);
    upscale(p, &src, w, h, ow, oh, FSR1_SHARPNESS_DEFAULT);
    long off = 0;
    for (int i = 0; i < ow * oh; i++) {
        uint32_t v = g.map[i];
        int r = (int)(v & 0xFF), gg = (int)(v >> 8 & 0xFF), b = (int)(v >> 16 & 0xFF);
        if (abs(r - 0xC0) > 1 || abs(gg - 0x80) > 1 || abs(b - 0x30) > 1) off++;
    }
    CHECK(off == 0, "%ld of %d pixels left the flat colour (RGBA C0 80 30)", off, ow * oh);
    image_free(&src);
}

/* source-unread: the pass never writes the image it is given (the guest's frame), and returns its
 * own image. */
static void cell_source(Fsr1Pass *p) {
    const int w = 64, h = 48, ow = 160, oh = 120;
    Source src;
    memset(&src, 0, sizeof(src));
    if (!image_make(&src, w, h, VK_FORMAT_R8G8B8A8_UNORM)) { CHECK(0, "source image"); return; }
    static uint32_t before[64 * 48];
    for (int i = 0; i < w * h; i++) {
        uint32_t x = (uint32_t)i * 2654435761u;
        before[i] = (x ^ x >> 15) | 0xFF000000u;
        g.map[i] = before[i];
    }
    source_fill(&src, w, h, 1);
    VkImage out = upscale(p, &src, w, h, ow, oh, FSR1_SHARPNESS_DEFAULT);
    CHECK(out != VK_NULL_HANDLE && out != src.image, "the pass returns its own image");
    image_read(src.image, w, h);
    CHECK(memcmp(before, g.map, sizeof(before)) == 0, "the pass changed the source image");
    image_free(&src);
}

static int cmp_double(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

/* cost: the GPU time of the pass at the showcase's size, 4x render scale to a 4K window. */
static void cell_cost(Fsr1Pass *p) {
    const int w = 1920, h = 1088, ow = 3840, oh = 2160;
    VkPhysicalDeviceProperties props;
    GetPhysicalDeviceProperties(g.physical, &props);
    if (props.limits.timestampPeriod <= 0.0f) { printf("fsr1-selftest: cost not measured (no timestamps)\n"); return; }
    Source src;
    memset(&src, 0, sizeof(src));
    if (!image_make(&src, w, h, VK_FORMAT_R8G8B8A8_UNORM)) { CHECK(0, "source image"); return; }
    for (int i = 0; i < 1024 * 1024; i++) g.map[i] = (uint32_t)i * 2654435761u | 0xFF000000u;
    source_fill(&src, 1024, 1024, 1);
    VkQueryPoolCreateInfo qi;
    memset(&qi, 0, sizeof(qi));
    qi.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = 2;
    VkQueryPool pool = VK_NULL_HANDLE;
    if (CreateQueryPool(g.device, &qi, NULL, &pool) != VK_SUCCESS) { CHECK(0, "query pool"); image_free(&src); return; }
    const char *names[2] = { "EASU", "EASU + RCAS" };
    const float sharp[2] = { FSR1_SHARPNESS_OFF, FSR1_SHARPNESS_DEFAULT };
    for (int k = 0; k < 2; k++) {
        double ms[15];
        int n = 0;
        for (int r = 0; r < 16; r++) {
            cmd_begin();
            CmdResetQueryPool(g.cb, pool, 0, 2);
            CmdWriteTimestamp(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool, 0);
            VkImage out = fsr1_record(p, g.cb, src.image, w, h, ow, oh, sharp[k]);
            CmdWriteTimestamp(g.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool, 1);
            cmd_end();
            CHECK(out != VK_NULL_HANDLE, "the 4K pass recorded");
            uint64_t t[2] = { 0, 0 };
            if (GetQueryPoolResults(g.device, pool, 0, 2, sizeof t, t, sizeof t[0],
                                    VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) != VK_SUCCESS) continue;
            if (r == 0) continue;  /* the first run builds the images */
            ms[n++] = (double)(t[1] - t[0]) * props.limits.timestampPeriod / 1e6;
        }
        if (n) {
            qsort(ms, (size_t)n, sizeof ms[0], cmp_double);
            printf("fsr1-selftest: cost of %s, 1920x1088 to 3840x2160 on %s: %.3f ms (median of %d)\n",
                   names[k], props.deviceName, ms[n / 2], n);
        }
    }
    DestroyQueryPool(g.device, pool, NULL);
    image_free(&src);
}

static uint32_t *read_spirv(const char *path, size_t *bytes) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint32_t *code = n > 0 && n % 4 == 0 ? (uint32_t *)malloc((size_t)n) : NULL;
    if (code && fread(code, 1, (size_t)n, f) != (size_t)n) { free(code); code = NULL; }
    fclose(f);
    *bytes = code ? (size_t)n : 0;
    return code;
}

int main(int argc, char **argv) {
    char why[160] = "";
    uint32_t *mutant = NULL;
    size_t mutant_bytes = 0;
    if (argc >= 3 && strcmp(argv[1], "--easu-spv") == 0) {
        mutant = read_spirv(argv[2], &mutant_bytes);
        if (!mutant) { printf("fsr1-selftest: cannot read %s\n", argv[2]); return 2; }
        s_mutant = 1;
    }
    int up = gpu_open(why, sizeof(why));
    if (up <= 0) {
        printf("SKIP fsr1-selftest: %s\n", why[0] ? why : "the device could not be set up");
        gpu_close();
        free(mutant);
        return up == 0 ? 77 : 1;
    }
    Fsr1Init init;
    memset(&init, 0, sizeof(init));
    init.get_instance_proc_addr = g.gipa;
    init.instance = g.instance;
    init.physical_device = g.physical;
    init.device = g.device;
    init.queue_family = g.family;
    init.easu_spirv = mutant;
    init.easu_spirv_bytes = mutant_bytes;
    Fsr1Pass *p = fsr1_create(&init, why, sizeof(why));
    CHECK(p != NULL, "fsr1_create: %s", why);
    CHECK(fsr1_parse_mode(NULL) == 0 && fsr1_parse_mode("") == 0 && fsr1_parse_mode("off") == 0 &&
          fsr1_parse_mode("fsr1") == 1 && fsr1_parse_mode("FSR") == -1, "SR_PRESENT_UPSCALE values");
    if (p) {
        cell_edge(p, 0);
        cell_edge(p, 1);
        if (!s_mutant) {
            cell_flat(p);
            cell_source(p);
            cell_cost(p);
        }
        fsr1_destroy(p);
    }
    gpu_close();
    free(mutant);
    printf(s_fail ? "fsr1-selftest: FAIL\n" : "fsr1-selftest: PASS\n");
    return s_fail ? 1 : 0;
}
