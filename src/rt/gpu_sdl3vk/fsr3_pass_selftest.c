/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr3_pass_selftest.c - FSR 3's upscaler (fsr3_pass.c) on a headless Vulkan device, under the
 * validation layers with synchronisation validation when they are installed. Links the pass, the
 * host and the C library alone.
 *
 * The scenes are drawn on the CPU as the GE's raster draws them: one point sample at each pixel
 * centre of the 512 x 272 canvas, the scene moved by the clip-space offset fsr3_jitter_clip_offset
 * gives for that viewport, and FSR's render size the 480 x 272 rectangle at its origin. Vectors are
 * prev - cur in render pixels, unjittered; FSR's depth is k / w, inverted and infinite. The truth is
 * the scene box-filtered at display resolution (4 x 4 samples a pixel), and the spatial baseline a
 * bilinear upscale of the unjittered render (FSR 1 is not on main; this baseline is weaker, so the
 * bars below are measured against it, not against FSR 1).
 *
 *   P-0  the eight pipelines, at five size pairs, with no validation message; refusals
 *   F-S  a static thin-line quad, 32 frames, ratios 2 and 1: the raster moves by the jitter FSR is
 *        told, to 0.01 px, and FSR's error is well below the baseline's
 *   F-M  a quad moving 3.5 px a frame diagonally with exact vectors: no trail behind it
 *   F-R  a translucent quad over moving content, its pixels in the reactive input: over a whole
 *        phase cycle its edges streak far less than with the reactive input unbound
 *   F-T  a moving quad whose vectors are wrongly zero, marked T&C 0.9: its ghost stays far below the
 *        ghost with T&C ignored, and a static region stays stable; the grades on a scrolling
 *        texture are recorded
 *   F-C  16 frames of one scene, then another with reset: the first frame of the second is as good
 *        as a spatial upscale and keeps nothing of the first
 *   F-D  a near quad moving across a far one, under full, partial and inverted viewport z mappings:
 *        the pixels it uncovers show the far quad
 *
 * usage: fsr3_pass_selftest [--cell NAME]...    (default: every cell)
 * Exit 77 (SKIP) without a Vulkan loader or device, without the features the pass needs, or, for
 * P-0, without the validation layer.
 */
#include "fsr3_pass.h"
#include "fsr3_jitter.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define CANVAS_W 512 /* the GE's canvas, FB_W */
#define CANVAS_H 272
#define RENDER_W 480 /* FSR's render size: the display rectangle inside the canvas */
#define RENDER_H 272
#define MAX_DISPLAY_W 960
#define MAX_DISPLAY_H 544
#define DEPTH_K 1.0     /* FSR depth is k / w */
#define SS 4            /* truth samples per display pixel and axis */

static int s_fail, s_skip;
#define CHECK(cond, ...) do { \
        if (!(cond)) { s_fail = 1; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
                       printf(__VA_ARGS__); printf("\n"); } \
    } while (0)

/* ---- the device ------------------------------------------------------------------------------ */

typedef struct {
    PFN_vkGetInstanceProcAddr gipa;
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    int validation;
    VkPhysicalDevice physical;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkBuffer up, down;
    VkDeviceMemory up_memory, down_memory;
    unsigned char *up_map, *down_map;
} Gpu;
static Gpu g;
static unsigned s_messages; /* validation messages so far */

#define GPU_FN(name) static PFN_vk##name name
GPU_FN(CreateInstance); GPU_FN(DestroyInstance); GPU_FN(EnumerateInstanceLayerProperties);
GPU_FN(EnumerateInstanceExtensionProperties); GPU_FN(EnumeratePhysicalDevices);
GPU_FN(GetPhysicalDeviceQueueFamilyProperties); GPU_FN(GetPhysicalDeviceMemoryProperties);
GPU_FN(GetPhysicalDeviceProperties);
GPU_FN(CreateDevice); GPU_FN(DestroyDevice); GPU_FN(GetDeviceProcAddr); GPU_FN(GetDeviceQueue);
GPU_FN(CreateCommandPool); GPU_FN(DestroyCommandPool); GPU_FN(AllocateCommandBuffers);
GPU_FN(CreateFence); GPU_FN(DestroyFence); GPU_FN(WaitForFences); GPU_FN(ResetFences);
GPU_FN(CreateImage); GPU_FN(DestroyImage); GPU_FN(GetImageMemoryRequirements);
GPU_FN(CreateImageView); GPU_FN(DestroyImageView);
GPU_FN(CreateBuffer); GPU_FN(DestroyBuffer); GPU_FN(GetBufferMemoryRequirements);
GPU_FN(AllocateMemory); GPU_FN(FreeMemory); GPU_FN(BindImageMemory); GPU_FN(BindBufferMemory);
GPU_FN(MapMemory); GPU_FN(BeginCommandBuffer); GPU_FN(EndCommandBuffer); GPU_FN(ResetCommandBuffer);
GPU_FN(CmdPipelineBarrier); GPU_FN(CmdCopyImageToBuffer); GPU_FN(CmdCopyBufferToImage); GPU_FN(CmdClearColorImage);
GPU_FN(QueueSubmit); GPU_FN(DeviceWaitIdle);
GPU_FN(CreateShaderModule); GPU_FN(DestroyShaderModule); GPU_FN(CreateDescriptorSetLayout);
GPU_FN(DestroyDescriptorSetLayout); GPU_FN(CreatePipelineLayout); GPU_FN(DestroyPipelineLayout);
GPU_FN(CreateComputePipelines); GPU_FN(DestroyPipeline); GPU_FN(CreateDescriptorPool);
GPU_FN(DestroyDescriptorPool); GPU_FN(AllocateDescriptorSets); GPU_FN(UpdateDescriptorSets);
GPU_FN(CmdBindPipeline); GPU_FN(CmdBindDescriptorSets); GPU_FN(CmdDispatch);
GPU_FN(CreateDebugUtilsMessengerEXT); GPU_FN(DestroyDebugUtilsMessengerEXT);
#undef GPU_FN

static int s_canary; /* the canary's expected hazard: counted, not printed */
static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                 VkDebugUtilsMessageTypeFlagsEXT type,
                                                 const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)severity;
    (void)type;
    (void)user;
    if (s_canary) {
        s_messages++;
        return VK_FALSE;
    }
    if (s_messages++ < 12) printf("validation: %s\n", data->pMessage ? data->pMessage : "(no text)");
    return VK_FALSE;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    GetPhysicalDeviceMemoryProperties(g.physical, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

static int has_layer(const char *name) {
    VkLayerProperties lp[64];
    uint32_t n = 64;
    if (!EnumerateInstanceLayerProperties || EnumerateInstanceLayerProperties(&n, lp) < VK_SUCCESS) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(lp[i].layerName, name)) return 1;
    return 0;
}

static int has_extension(const char *layer, const char *name) {
    VkExtensionProperties ep[128];
    uint32_t n = 128;
    if (EnumerateInstanceExtensionProperties(layer, &n, ep) < VK_SUCCESS) return 0;
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(ep[i].extensionName, name)) return 1;
    return 0;
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *b, VkDeviceMemory *m,
                       unsigned char **map) {
    VkBufferCreateInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    if (CreateBuffer(g.device, &bi, NULL, b) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    GetBufferMemoryRequirements(g.device, *b, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    return mai.memoryTypeIndex != UINT32_MAX && AllocateMemory(g.device, &mai, NULL, m) == VK_SUCCESS &&
           BindBufferMemory(g.device, *b, *m, 0) == VK_SUCCESS &&
           MapMemory(g.device, *m, 0, VK_WHOLE_SIZE, 0, (void **)map) == VK_SUCCESS;
}

#define UP_BYTES ((VkDeviceSize)CANVAS_W * CANVAS_H * 14u) /* colour 4, depth 4, vector 4, masks 1 + 1 */
#define DOWN_BYTES ((VkDeviceSize)MAX_DISPLAY_W * MAX_DISPLAY_H * 4u)

/* 1, or 0 with the reason for a skip. */
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
    EnumerateInstanceLayerProperties =
        (PFN_vkEnumerateInstanceLayerProperties)g.gipa(VK_NULL_HANDLE, "vkEnumerateInstanceLayerProperties");
    EnumerateInstanceExtensionProperties = (PFN_vkEnumerateInstanceExtensionProperties)g.gipa(
        VK_NULL_HANDLE, "vkEnumerateInstanceExtensionProperties");
    if (!CreateInstance || !EnumerateInstanceExtensionProperties) { snprintf(why, cap, "no vkCreateInstance"); return 0; }

    const char *layer = "VK_LAYER_KHRONOS_validation";
    g.validation = has_layer(layer) && has_extension(NULL, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const int sync = g.validation && has_extension(layer, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    const int settings = sync && has_extension(layer, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    const char *extensions[3] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME,
                                  VK_EXT_LAYER_SETTINGS_EXTENSION_NAME };
    /* Without the shader-access heuristic, synchronisation validation does not see a dispatch's
     * reads and writes through its descriptors, so a missing barrier between two passes goes
     * unreported (measured on SDK 1.4.363). */
    const VkBool32 on = VK_TRUE;
    VkLayerSettingEXT layer_settings[2] = {
        { "VK_LAYER_KHRONOS_validation", "validate_sync", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on },
        { "VK_LAYER_KHRONOS_validation", "syncval_shader_accesses_heuristic", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &on },
    };
    VkLayerSettingsCreateInfoEXT lsci;
    memset(&lsci, 0, sizeof(lsci));
    lsci.sType = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT;
    lsci.settingCount = 2;
    lsci.pSettings = layer_settings;
    VkValidationFeatureEnableEXT enables[1] = { VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT };
    VkValidationFeaturesEXT vf;
    memset(&vf, 0, sizeof(vf));
    vf.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
    vf.pNext = settings ? &lsci : NULL;
    vf.enabledValidationFeatureCount = 1;
    vf.pEnabledValidationFeatures = enables;
    VkDebugUtilsMessengerCreateInfoEXT mci;
    memset(&mci, 0, sizeof(mci));
    mci.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
    mci.pfnUserCallback = on_message;
    mci.pNext = sync ? &vf : NULL;

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "fsr3_pass_selftest", 1, NULL, 0,
                              VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    if (g.validation) {
        ici.pNext = &mci;
        ici.enabledLayerCount = 1;
        ici.ppEnabledLayerNames = &layer;
        ici.enabledExtensionCount = settings ? 3u : sync ? 2u : 1u;
        ici.ppEnabledExtensionNames = extensions;
    }
    if (CreateInstance(&ici, NULL, &g.instance) != VK_SUCCESS) {
        snprintf(why, cap, "no usable Vulkan driver");
        return 0;
    }
    printf("validation: %s\n", !g.validation ? "off (VK_LAYER_KHRONOS_validation or VK_EXT_debug_utils not found)"
                               : settings   ? "on, with synchronisation validation of shader accesses"
                               : sync       ? "on, with synchronisation validation (no VK_EXT_layer_settings: "
                                              "the layer's own default decides whether shader accesses are "
                                              "checked, and the canaries measure it)"
                                            : "on, without synchronisation validation");
#define INST(name) name = (PFN_vk##name)g.gipa(g.instance, "vk" #name)
    INST(DestroyInstance); INST(EnumeratePhysicalDevices); INST(GetPhysicalDeviceQueueFamilyProperties);
    INST(GetPhysicalDeviceMemoryProperties); INST(GetPhysicalDeviceProperties); INST(CreateDevice);
    INST(GetDeviceProcAddr); INST(CreateDebugUtilsMessengerEXT); INST(DestroyDebugUtilsMessengerEXT);
#undef INST
    mci.pNext = NULL; /* the chain above belongs to instance creation; the messenger takes none */
    if (g.validation && (!CreateDebugUtilsMessengerEXT ||
                         CreateDebugUtilsMessengerEXT(g.instance, &mci, NULL, &g.messenger) != VK_SUCCESS)) {
        snprintf(why, cap, "vkCreateDebugUtilsMessengerEXT failed");
        return 0;
    }
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
            if (qf[q].queueFlags & VK_QUEUE_COMPUTE_BIT) {
                g.physical = devs[d];
                g.family = q;
                break;
            }
    }
    if (!g.physical) { snprintf(why, cap, "no compute queue"); return 0; }
    VkPhysicalDeviceProperties props;
    GetPhysicalDeviceProperties(g.physical, &props);
    printf("device: %s (driver 0x%x)\n", props.deviceName, props.driverVersion);
    const char *missing = fsr3_pass_unavailable(g.gipa, g.instance, g.physical, VK_FORMAT_R8_UNORM);
    if (missing) { snprintf(why, cap, "the device lacks %s", missing); return 0; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceFeatures features;
    memset(&features, 0, sizeof(features));
    features.shaderStorageImageReadWithoutFormat = VK_TRUE;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.pEnabledFeatures = &features;
    if (CreateDevice(g.physical, &dci, NULL, &g.device) != VK_SUCCESS) {
        snprintf(why, cap, "vkCreateDevice failed");
        return 0;
    }
#define DEV(name) name = (PFN_vk##name)GetDeviceProcAddr(g.device, "vk" #name)
    DEV(DestroyDevice); DEV(GetDeviceQueue); DEV(CreateCommandPool); DEV(DestroyCommandPool);
    DEV(AllocateCommandBuffers); DEV(CreateFence); DEV(DestroyFence); DEV(WaitForFences);
    DEV(ResetFences); DEV(CreateImage); DEV(DestroyImage); DEV(GetImageMemoryRequirements);
    DEV(CreateImageView); DEV(DestroyImageView);
    DEV(CreateBuffer); DEV(DestroyBuffer); DEV(GetBufferMemoryRequirements); DEV(AllocateMemory);
    DEV(FreeMemory); DEV(BindImageMemory); DEV(BindBufferMemory); DEV(MapMemory);
    DEV(BeginCommandBuffer); DEV(EndCommandBuffer); DEV(ResetCommandBuffer); DEV(CmdPipelineBarrier);
    DEV(CmdCopyImageToBuffer); DEV(CmdCopyBufferToImage); DEV(CmdClearColorImage); DEV(QueueSubmit);
    DEV(DeviceWaitIdle);
    DEV(CreateShaderModule); DEV(DestroyShaderModule); DEV(CreateDescriptorSetLayout);
    DEV(DestroyDescriptorSetLayout); DEV(CreatePipelineLayout); DEV(DestroyPipelineLayout);
    DEV(CreateComputePipelines); DEV(DestroyPipeline); DEV(CreateDescriptorPool);
    DEV(DestroyDescriptorPool); DEV(AllocateDescriptorSets); DEV(UpdateDescriptorSets);
    DEV(CmdBindPipeline); DEV(CmdBindDescriptorSets); DEV(CmdDispatch);
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
        CreateFence(g.device, &fci, NULL, &g.fence) != VK_SUCCESS)
        return -1;
    if (!make_buffer(UP_BYTES, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &g.up, &g.up_memory, &g.up_map) ||
        !make_buffer(DOWN_BYTES, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &g.down, &g.down_memory, &g.down_map))
        return -1;
    return 1;
}

static void gpu_close(void) {
    if (g.device) {
        DeviceWaitIdle(g.device);
        if (g.up) DestroyBuffer(g.device, g.up, NULL);
        if (g.up_memory) FreeMemory(g.device, g.up_memory, NULL);
        if (g.down) DestroyBuffer(g.device, g.down, NULL);
        if (g.down_memory) FreeMemory(g.device, g.down_memory, NULL);
        if (g.fence) DestroyFence(g.device, g.fence, NULL);
        if (g.pool) DestroyCommandPool(g.device, g.pool, NULL);
        DestroyDevice(g.device, NULL);
    }
    if (g.messenger) DestroyDebugUtilsMessengerEXT(g.instance, g.messenger, NULL);
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

/* Every earlier compute or transfer access before every later one. */
static void barrier_all(void) {
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
                           VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                               VK_ACCESS_TRANSFER_WRITE_BIT };
    const VkPipelineStageFlags stages = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    CmdPipelineBarrier(g.cb, stages, stages, 0, 1, &mb, 0, NULL, 0, NULL);
}

typedef struct { VkImage image; VkDeviceMemory memory; VkImageView view; } Tex;

static int tex_make(Tex *t, uint32_t w, uint32_t h, VkFormat format, VkImageUsageFlags usage) {
    VkImageCreateInfo ii;
    memset(&ii, 0, sizeof(ii));
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = format;
    ii.extent.width = w;
    ii.extent.height = h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    if (CreateImage(g.device, &ii, NULL, &t->image) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    GetImageMemoryRequirements(g.device, t->image, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size,
                                 memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    if (mai.memoryTypeIndex == UINT32_MAX || AllocateMemory(g.device, &mai, NULL, &t->memory) != VK_SUCCESS ||
        BindImageMemory(g.device, t->image, t->memory, 0) != VK_SUCCESS)
        return 0;
    VkImageViewCreateInfo vi;
    memset(&vi, 0, sizeof(vi));
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = t->image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (CreateImageView(g.device, &vi, NULL, &t->view) != VK_SUCCESS) return 0;
    cmd_begin();
    VkImageMemoryBarrier b;
    memset(&b, 0, sizeof(b));
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = t->image;
    b.subresourceRange = vi.subresourceRange;
    CmdPipelineBarrier(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, NULL, 0, NULL, 1,
                       &b);
    cmd_end();
    return 1;
}

static void tex_free(Tex *t) {
    if (t->view) DestroyImageView(g.device, t->view, NULL);
    if (t->image) DestroyImage(g.device, t->image, NULL);
    if (t->memory) FreeMemory(g.device, t->memory, NULL);
    memset(t, 0, sizeof(*t));
}

/* Synchronisation validation is what proves the pass's barriers, so its presence is measured, not
 * assumed: two clears of one image with no barrier between them are a write-after-write hazard. */
static int g_syncval;
static void syncval_canary(void) {
    Tex t;
    memset(&t, 0, sizeof(t));
    if (!g.validation || !tex_make(&t, 4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)) {
        tex_free(&t);
        return;
    }
    const unsigned before = s_messages;
    VkClearColorValue v;
    memset(&v, 0, sizeof(v));
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    s_canary = 1;
    cmd_begin();
    CmdClearColorImage(g.cb, t.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    CmdClearColorImage(g.cb, t.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
    cmd_end();
    s_canary = 0;
    g_syncval = s_messages > before;
    s_messages = before;
    tex_free(&t);
    printf("synchronisation validation: %s\n", g_syncval ? "active (the canary's hazard was reported)"
                                                         : "NOT active (the canary's hazard was not reported)");
}

/* The barriers between passes are proved only if the layer checks what a dispatch writes through
 * its descriptors. Newer layers do that only with syncval_shader_accesses_heuristic (enabled in
 * gpu_open where VK_EXT_layer_settings exists); older ones did it by default. So it is measured:
 * a dispatch writes a storage image, then a clear writes it with no barrier between them.
 *
 * The shader, assembled by hand (SPIR-V 1.0), is
 *   #version 450
 *   layout(local_size_x = 1) in;
 *   layout(set = 0, binding = 0, rgba8) uniform writeonly image2D img;
 *   void main() { imageStore(img, ivec2(0), vec4(0)); } */
static const uint32_t k_canary_spirv[] = {
    0x07230203, 0x00010000, 0, 17, 0,              /* magic, 1.0, generator, bound 17, schema */
    0x00020011, 1,                                 /* OpCapability Shader */
    0x0003000E, 0, 1,                              /* OpMemoryModel Logical GLSL450 */
    0x0005000F, 5, 1, 0x6E69616D, 0,               /* OpEntryPoint GLCompute %1 "main" */
    0x00060010, 1, 17, 1, 1, 1,                    /* OpExecutionMode %1 LocalSize 1 1 1 */
    0x00040047, 2, 34, 0,                          /* OpDecorate %2 DescriptorSet 0 */
    0x00040047, 2, 33, 0,                          /* OpDecorate %2 Binding 0 */
    0x00030047, 2, 25,                             /* OpDecorate %2 NonReadable */
    0x00020013, 3,                                 /* %3 = OpTypeVoid */
    0x00030021, 4, 3,                              /* %4 = OpTypeFunction %3 */
    0x00030016, 5, 32,                             /* %5 = OpTypeFloat 32 */
    0x00090019, 6, 5, 1, 0, 0, 0, 2, 4,            /* %6 = OpTypeImage %5 2D, storage, Rgba8 */
    0x00040020, 7, 0, 6,                           /* %7 = OpTypePointer UniformConstant %6 */
    0x0004003B, 7, 2, 0,                           /* %2 = OpVariable %7 UniformConstant */
    0x00040015, 8, 32, 1,                          /* %8 = OpTypeInt 32 signed */
    0x00040017, 9, 8, 2,                           /* %9 = OpTypeVector %8 2 */
    0x0004002B, 8, 10, 0,                          /* %10 = OpConstant %8 0 */
    0x0005002C, 9, 11, 10, 10,                     /* %11 = OpConstantComposite %9 %10 %10 */
    0x00040017, 12, 5, 4,                          /* %12 = OpTypeVector %5 4 */
    0x0004002B, 5, 13, 0,                          /* %13 = OpConstant %5 0.0 */
    0x0007002C, 12, 14, 13, 13, 13, 13,            /* %14 = OpConstantComposite %12 %13 x4 */
    0x00050036, 3, 1, 0, 4,                        /* %1 = OpFunction %3 None %4 */
    0x000200F8, 15,                                /* %15 = OpLabel */
    0x0004003D, 6, 16, 2,                          /* %16 = OpLoad %6 %2 */
    0x00040063, 16, 11, 14,                        /* OpImageWrite %16 %11 %14 */
    0x000100FD,                                    /* OpReturn */
    0x00010038,                                    /* OpFunctionEnd */
};

static int g_syncval_shader;
static void shader_canary(void) {
    if (!g_syncval) return;
    Tex t;
    memset(&t, 0, sizeof(t));
    VkShaderModule module = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipe = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    const unsigned made = s_messages;
    int ok = tex_make(&t, 4, 4, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    if (ok) {
        VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof(k_canary_spirv),
                                         k_canary_spirv };
        ok = CreateShaderModule(g.device, &smi, NULL, &module) == VK_SUCCESS;
    }
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    if (ok) {
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b };
        ok = CreateDescriptorSetLayout(g.device, &li, NULL, &dsl) == VK_SUCCESS;
    }
    if (ok) {
        VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 0, NULL };
        ok = CreatePipelineLayout(g.device, &pli, NULL, &layout) == VK_SUCCESS;
    }
    if (ok) {
        VkComputePipelineCreateInfo ci;
        memset(&ci, 0, sizeof(ci));
        ci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        ci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        ci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        ci.stage.module = module;
        ci.stage.pName = "main";
        ci.layout = layout;
        ok = CreateComputePipelines(g.device, VK_NULL_HANDLE, 1, &ci, NULL, &pipe) == VK_SUCCESS;
    }
    if (ok) {
        VkDescriptorPoolSize size = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 };
        VkDescriptorPoolCreateInfo pi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &size };
        ok = CreateDescriptorPool(g.device, &pi, NULL, &dpool) == VK_SUCCESS;
    }
    if (ok) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpool, 1, &dsl };
        ok = AllocateDescriptorSets(g.device, &ai, &set) == VK_SUCCESS;
    }
    if (ok && s_messages == made) {
        VkDescriptorImageInfo ii = { VK_NULL_HANDLE, t.view, VK_IMAGE_LAYOUT_GENERAL };
        VkWriteDescriptorSet w;
        memset(&w, 0, sizeof(w));
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &ii;
        UpdateDescriptorSets(g.device, 1, &w, 0, NULL);
        VkClearColorValue v;
        memset(&v, 0, sizeof(v));
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        const unsigned before = s_messages;
        s_canary = 1;
        cmd_begin();
        CmdBindPipeline(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
        CmdBindDescriptorSets(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
        CmdDispatch(g.cb, 1, 1, 1);
        CmdClearColorImage(g.cb, t.image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &range);
        cmd_end();
        s_canary = 0;
        g_syncval_shader = s_messages > before;
        s_messages = before;
    }
    /* Setting the canary up must itself be clean: a message here is a bug in this file. */
    CHECK(s_messages == made, "the dispatch canary's setup gave %u validation messages", s_messages - made);
    if (dpool) DestroyDescriptorPool(g.device, dpool, NULL);
    if (pipe) DestroyPipeline(g.device, pipe, NULL);
    if (layout) DestroyPipelineLayout(g.device, layout, NULL);
    if (dsl) DestroyDescriptorSetLayout(g.device, dsl, NULL);
    if (module) DestroyShaderModule(g.device, module, NULL);
    tex_free(&t);
    printf("synchronisation validation of shader accesses: %s\n",
           g_syncval_shader ? "active (the dispatch canary's hazard was reported)"
           : ok && s_messages == made ? "NOT active (the dispatch canary's hazard was not reported)"
                                      : "not measured (the dispatch canary could not be set up)");
}

static void copy_in(const Tex *t, VkDeviceSize offset) {
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.bufferOffset = offset;
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = CANVAS_W;
    c.imageExtent.height = CANVAS_H;
    c.imageExtent.depth = 1;
    CmdCopyBufferToImage(g.cb, g.up, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
}

/* ---- the scenes ------------------------------------------------------------------------------ */

enum { TEX_LINES, TEX_GREEN, TEX_RED, TEX_BLUE, TEX_GREY, TEX_YELLOW };

static double fract(double v) {
    return v - floor(v);
}

static const double TAU = 6.283185307179586;

/* Source-owned textures, in canvas pixels from the surface's origin. */
static void texel(int tex, double u, double v, double rgb[3]) {
    switch (tex) {
    case TEX_LINES: { /* two families of thin dark lines, tilted, under the display's Nyquist limit */
        const double a = fract((u * 0.99863 + v * 0.05234) / 3.3) < 0.7 / 3.3;
        const double b = fract((v * 0.99939 - u * 0.03490) / 4.1) < 0.6 / 4.1;
        const double k = (a || b) ? 0.1 : 0.85;
        rgb[0] = k; rgb[1] = k; rgb[2] = k * 0.95 + 0.03;
        return;
    }
    case TEX_GREEN:
        rgb[0] = 0.2 + 0.15 * sin(TAU * (u + v) / 11.0);
        rgb[1] = 0.5 + 0.3 * sin(TAU * u / 7.3) * sin(TAU * v / 5.9) + 0.12 * ((((int)floor(u / 6) + (int)floor(v / 6)) & 1) ? 1 : -1);
        rgb[2] = 0.3;
        return;
    case TEX_RED:
        rgb[0] = 0.7 + 0.25 * sin(TAU * u / 6.1) * sin(TAU * v / 8.3);
        rgb[1] = 0.15 + 0.1 * ((((int)floor(u / 5) + (int)floor(v / 5)) & 1) ? 1 : -1);
        rgb[2] = 0.25 + 0.2 * sin(TAU * (u - v) / 9.7);
        return;
    case TEX_BLUE:
        rgb[0] = 0.1 + 0.08 * sin(TAU * v / 4.7);
        rgb[1] = 0.25 + 0.2 * sin(TAU * (u * 0.6 + v) / 6.7);
        rgb[2] = 0.75 + 0.2 * sin(TAU * u / 5.3) + 0.05 * ((((int)floor(u / 7) + (int)floor(v / 7)) & 1) ? 1 : -1);
        return;
    case TEX_YELLOW:
        rgb[0] = 1.0; rgb[1] = 0.9; rgb[2] = 0.2;
        return;
    default:
        rgb[0] = rgb[1] = rgb[2] = 0.5;
        return;
    }
}

typedef struct {
    double x0, y0, x1, y1; /* canvas pixels at t = 0 */
    double vx, vy;         /* pixels a frame */
    double w;              /* clip w: view depth */
    int tex;
    double alpha;          /* 1: opaque */
    double tc;             /* the T&C value its pixels write */
    int zero_vector;       /* its vectors are wrongly 0 (F-T) */
    double scroll;         /* texture scroll in x, pixels a frame; the geometry is still */
} Quad;

enum { ZMAP_FULL, ZMAP_PARTIAL, ZMAP_INVERTED };

typedef struct {
    int bg_tex;
    double bg_w, bg_tc;
    int zmap;
    int nq;
    Quad q[4];
} Scene;

typedef struct {
    double c[3];
    double depth;   /* FSR's: k / w */
    double mv[2];   /* prev - cur, render pixels */
    double reactive, tc;
} Px;

/* The GE's window z for clip w under a GL-style projection (near 1, far 100) and a viewport z
 * mapping, as visibility sees it; FSR's depth never uses it. */
static double window_z(double w, int zmap) {
    const double n = 1.0, f = 100.0;
    const double ndc = (f + n) / (f - n) - 2.0 * f * n / ((f - n) * w);
    switch (zmap) {
    case ZMAP_PARTIAL: return ndc * 10000.0 + 30000.0;
    case ZMAP_INVERTED: return ndc * -32767.5 + 32767.5;
    default: return ndc * 32767.5 + 32767.5;
    }
}

/* FSR's depth: k / w, whatever the window z the GE wrote (design 2.3). */
static double fsr_depth(double w, double window) {
    (void)window;
    return DEPTH_K / w;
}

static int nearer(double z, double than, int zmap) {
    return zmap == ZMAP_INVERTED ? z > than : z < than;
}

static int inside(const Quad *q, double x, double y, int t) {
    return x >= q->x0 + q->vx * t && x < q->x1 + q->vx * t && y >= q->y0 + q->vy * t && y < q->y1 + q->vy * t;
}

static void scene_at(const Scene *s, double x, double y, int t, Px *o) {
    texel(s->bg_tex, x, y, o->c);
    double z = window_z(s->bg_w, s->zmap);
    o->depth = fsr_depth(s->bg_w, z);
    o->mv[0] = o->mv[1] = 0.0;
    o->reactive = 0.0;
    o->tc = s->bg_tc;
    for (int i = 0; i < s->nq; i++) {
        const Quad *q = &s->q[i];
        if (!inside(q, x, y, t)) continue;
        const double qz = window_z(q->w, s->zmap);
        if (!nearer(qz, z, s->zmap)) continue;
        double rgb[3];
        texel(q->tex, x - (q->x0 + q->vx * t) + q->scroll * t, y - (q->y0 + q->vy * t), rgb);
        if (q->alpha >= 1.0) {
            const double mvx = -q->vx, mvy = -q->vy;
            memcpy(o->c, rgb, sizeof(rgb));
            z = qz;
            o->depth = fsr_depth(q->w, qz);
            o->mv[0] = q->zero_vector ? 0.0 : mvx;
            o->mv[1] = q->zero_vector ? 0.0 : mvy;
            o->reactive = 0.0;
            o->tc = q->tc;
        } else { /* blended: colour and the masks only */
            for (int c = 0; c < 3; c++) o->c[c] = o->c[c] * (1.0 - q->alpha) + rgb[c] * q->alpha;
            o->reactive = fmax(o->reactive, fmin(0.9, q->alpha));
            o->tc = fmax(o->tc, q->tc);
        }
    }
}

static uint8_t unorm8(double v) {
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    return (uint8_t)(v * 255.0 + 0.5);
}

/* ---- a run ----------------------------------------------------------------------------------- */

typedef struct {
    int dw, dh;
    Fsr3Pass *pass;
    Tex color, depth, mv, reactive, tc, out;
    Fsr3Jitter jitter;
    int bind_reactive, bind_tc;
    uint8_t *out_px;  /* the last output, dw * dh RGBA */
    float *truth;     /* dw * dh RGB, in [0, 1] */
    float *base;      /* the bilinear baseline, dw * dh RGB */
    float jx, jy;     /* the last jitter */
    double sx, sy;    /* the last raster offset, canvas pixels */
} Run;

static Fsr3HostDesc desc_for(int dw, int dh) {
    Fsr3HostDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = FSR3_HOST_ENABLE_DEPTH_INVERTED | FSR3_HOST_ENABLE_DEPTH_INFINITE;
    d.max_render_width = RENDER_W;
    d.max_render_height = RENDER_H;
    d.max_upscale_width = (uint32_t)dw;
    d.max_upscale_height = (uint32_t)dh;
    return d;
}

static Fsr3Pass *pass_new(Fsr3HostDesc desc, uint32_t frames, char *err, size_t cap) {
    Fsr3PassInit init;
    memset(&init, 0, sizeof(init));
    init.get_instance_proc_addr = g.gipa;
    init.instance = g.instance;
    init.physical_device = g.physical;
    init.device = g.device;
    init.frames_in_flight = frames;
    init.desc = desc;
    return fsr3_pass_create(&init, err, cap);
}

static int run_open(Run *r, int dw, int dh) {
    memset(r, 0, sizeof(*r));
    r->dw = dw;
    r->dh = dh;
    r->bind_reactive = r->bind_tc = 1;
    char err[160] = "";
    r->pass = pass_new(desc_for(dw, dh), 1, err, sizeof(err));
    if (!r->pass) { printf("FAIL fsr3_pass_create: %s\n", err); s_fail = 1; return 0; }
    const VkImageUsageFlags in = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    r->out_px = malloc((size_t)dw * dh * 4);
    r->truth = malloc((size_t)dw * dh * 3 * sizeof(float));
    r->base = malloc((size_t)dw * dh * 3 * sizeof(float));
    return r->out_px && r->truth && r->base &&
           tex_make(&r->color, CANVAS_W, CANVAS_H, VK_FORMAT_R8G8B8A8_UNORM, in) &&
           tex_make(&r->depth, CANVAS_W, CANVAS_H, VK_FORMAT_R32_SFLOAT, in) &&
           tex_make(&r->mv, CANVAS_W, CANVAS_H, VK_FORMAT_R16G16_SFLOAT, in) &&
           tex_make(&r->reactive, CANVAS_W, CANVAS_H, VK_FORMAT_R8_UNORM, in) &&
           tex_make(&r->tc, CANVAS_W, CANVAS_H, VK_FORMAT_R8_UNORM, in) &&
           tex_make(&r->out, (uint32_t)dw, (uint32_t)dh, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

static void run_close(Run *r) {
    fsr3_pass_destroy(r->pass);
    tex_free(&r->color);
    tex_free(&r->depth);
    tex_free(&r->mv);
    tex_free(&r->reactive);
    tex_free(&r->tc);
    tex_free(&r->out);
    free(r->out_px);
    free(r->truth);
    free(r->base);
}

/* The jittered raster of frame t into the upload buffer: the canvas moved by the clip-space offset
 * for the canvas viewport, sampled at pixel centres. */
static void raster(Run *r, const Scene *s, int t) {
    float o[2];
    fsr3_jitter_clip_offset(r->jx, r->jy, (float)CANVAS_W, (float)CANVAS_H, o);
    r->sx = o[0] * CANVAS_W / 2.0;
    r->sy = o[1] * CANVAS_H / 2.0;
    unsigned char *color = g.up_map;
    float *depth = (float *)(g.up_map + (size_t)CANVAS_W * CANVAS_H * 4);
    uint16_t *mv = (uint16_t *)(g.up_map + (size_t)CANVAS_W * CANVAS_H * 8);
    unsigned char *reactive = g.up_map + (size_t)CANVAS_W * CANVAS_H * 12;
    unsigned char *tc = reactive + (size_t)CANVAS_W * CANVAS_H;
    for (int y = 0; y < CANVAS_H; y++)
        for (int x = 0; x < CANVAS_W; x++) {
            Px p;
            scene_at(s, x + 0.5 - r->sx, y + 0.5 - r->sy, t, &p);
            const size_t i = (size_t)y * CANVAS_W + x;
            for (int c = 0; c < 3; c++) color[i * 4 + c] = unorm8(p.c[c]);
            color[i * 4 + 3] = 255;
            depth[i] = (float)p.depth;
            mv[i * 2] = (uint16_t)fsr3_f32_to_f16((float)p.mv[0]);
            mv[i * 2 + 1] = (uint16_t)fsr3_f32_to_f16((float)p.mv[1]);
            reactive[i] = unorm8(p.reactive);
            tc[i] = unorm8(p.tc);
        }
}

/* One FSR 3 frame of scene s at time t; the output lands in r->out_px. */
static int frame(Run *r, const Scene *s, int t, int reset) {
    if (fsr3_jitter_next(&r->jitter, reset, RENDER_W, r->dw, &r->jx, &r->jy) != 0) return 0;
    raster(r, s, t);
    Fsr3PassFrame f;
    memset(&f, 0, sizeof(f));
    f.color = r->color.view;
    f.depth = r->depth.view;
    f.motion_vectors = r->mv.view;
    f.reactive = r->bind_reactive ? r->reactive.view : VK_NULL_HANDLE;
    f.transparency_and_composition = r->bind_tc ? r->tc.view : VK_NULL_HANDLE;
    f.input_layout = VK_IMAGE_LAYOUT_GENERAL;
    f.output = r->out.view;
    f.slot = 0;
    f.params.jitter_x = r->jx;
    f.params.jitter_y = r->jy;
    f.params.motion_vector_scale_x = 1.0f;
    f.params.motion_vector_scale_y = 1.0f;
    f.params.render_width = RENDER_W;
    f.params.render_height = RENDER_H;
    f.params.upscale_width = (uint32_t)r->dw;
    f.params.upscale_height = (uint32_t)r->dh;
    f.params.frame_time_delta_ms = 1000.0f / 60.0f;
    f.params.pre_exposure = 1.0f;
    f.params.reset = reset;
    f.params.camera_near = (float)DEPTH_K;
    f.params.camera_far = 1000.0f;
    f.params.camera_fov_angle_vertical = 1.0471976f;
    f.params.view_space_to_meters_factor = 1.0f;

    cmd_begin();
    barrier_all(); /* the last frame's reads of the inputs and of the output's copy */
    copy_in(&r->color, 0);
    copy_in(&r->depth, (VkDeviceSize)CANVAS_W * CANVAS_H * 4);
    copy_in(&r->mv, (VkDeviceSize)CANVAS_W * CANVAS_H * 8);
    copy_in(&r->reactive, (VkDeviceSize)CANVAS_W * CANVAS_H * 12);
    copy_in(&r->tc, (VkDeviceSize)CANVAS_W * CANVAS_H * 13);
    barrier_all();
    const int rc = fsr3_pass_record(r->pass, g.cb, &f);
    barrier_all();
    VkBufferImageCopy c;
    memset(&c, 0, sizeof(c));
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)r->dw;
    c.imageExtent.height = (uint32_t)r->dh;
    c.imageExtent.depth = 1;
    CmdCopyImageToBuffer(g.cb, r->out.image, VK_IMAGE_LAYOUT_GENERAL, g.down, 1, &c);
    cmd_end();
    memcpy(r->out_px, g.down_map, (size_t)r->dw * r->dh * 4);
    CHECK(rc == 0, "fsr3_pass_record refused frame %d", t);
    return rc == 0;
}

/* The canvas position of a display pixel's sample (i, j) of SS x SS. */
static double canvas_x(const Run *r, int u, int i) {
    return (u + (i + 0.5) / SS) * RENDER_W / r->dw;
}
static double canvas_y(const Run *r, int v, int j) {
    return (v + (j + 0.5) / SS) * RENDER_H / r->dh;
}

static void truth(Run *r, const Scene *s, int t) {
    for (int v = 0; v < r->dh; v++)
        for (int u = 0; u < r->dw; u++) {
            double acc[3] = { 0, 0, 0 };
            for (int j = 0; j < SS; j++)
                for (int i = 0; i < SS; i++) {
                    Px p;
                    scene_at(s, canvas_x(r, u, i), canvas_y(r, v, j), t, &p);
                    for (int c = 0; c < 3; c++) acc[c] += p.c[c];
                }
            for (int c = 0; c < 3; c++) r->truth[((size_t)v * r->dw + u) * 3 + c] = (float)(acc[c] / (SS * SS));
        }
}

/* The spatial baseline: the unjittered render of frame t, upscaled bilinearly. */
static void baseline(Run *r, const Scene *s, int t) {
    static double img[RENDER_H][RENDER_W][3];
    for (int y = 0; y < RENDER_H; y++)
        for (int x = 0; x < RENDER_W; x++) {
            Px p;
            scene_at(s, x + 0.5, y + 0.5, t, &p);
            for (int c = 0; c < 3; c++) img[y][x][c] = unorm8(p.c[c]) / 255.0;
        }
    for (int v = 0; v < r->dh; v++)
        for (int u = 0; u < r->dw; u++) {
            double fx = (u + 0.5) * RENDER_W / r->dw - 0.5, fy = (v + 0.5) * RENDER_H / r->dh - 0.5;
            fx = fx < 0 ? 0 : fx > RENDER_W - 1 ? RENDER_W - 1 : fx;
            fy = fy < 0 ? 0 : fy > RENDER_H - 1 ? RENDER_H - 1 : fy;
            const int x0 = (int)fx, y0 = (int)fy;
            const int x1 = x0 + 1 < RENDER_W ? x0 + 1 : x0, y1 = y0 + 1 < RENDER_H ? y0 + 1 : y0;
            const double ax = fx - x0, ay = fy - y0;
            for (int c = 0; c < 3; c++)
                r->base[((size_t)v * r->dw + u) * 3 + c] =
                    (float)((img[y0][x0][c] * (1 - ax) + img[y0][x1][c] * ax) * (1 - ay) +
                            (img[y1][x0][c] * (1 - ax) + img[y1][x1][c] * ax) * ay);
        }
}

/* A region of display pixels, decided at the pixel's centre in canvas pixels. */
typedef int (*Region)(const Run *r, const Scene *s, int t, double x, double y);

/* Mean absolute error in 8-bit levels over a region, of the output (or of `alt`) against the truth. */
static double mae(const Run *r, const float *alt, const Scene *s, int t, Region in, unsigned *count) {
    double sum = 0;
    unsigned n = 0;
    for (int v = 0; v < r->dh; v++)
        for (int u = 0; u < r->dw; u++) {
            const double x = (u + 0.5) * RENDER_W / r->dw, y = (v + 0.5) * RENDER_H / r->dh;
            if (in && !in(r, s, t, x, y)) continue;
            const size_t i = (size_t)v * r->dw + u;
            for (int c = 0; c < 3; c++) {
                const double got = alt ? alt[i * 3 + c] * 255.0 : r->out_px[i * 4 + c];
                sum += fabs(got - r->truth[i * 3 + c] * 255.0);
            }
            n++;
        }
    if (count) *count = n;
    return n ? sum / (3.0 * n) : 0.0;
}

/* ---- P-0 ------------------------------------------------------------------------------------- */

static void cell_p0(void) {
    if (!g.validation) {
        printf("SKIP P-0 (no validation layer: pipeline creation is not checked)\n");
        s_skip = 1;
        return;
    }
    static const uint32_t sizes[][4] = {
        { 480, 272, 960, 544 }, { 480, 272, 480, 272 }, { 480, 272, 1440, 816 }, { 333, 187, 1000, 561 }, { 2, 2, 2, 2 },
    };
    const unsigned before = s_messages;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        Fsr3HostDesc d = desc_for((int)sizes[i][2], (int)sizes[i][3]);
        d.max_render_width = sizes[i][0];
        d.max_render_height = sizes[i][1];
        char err[160] = "";
        Fsr3Pass *p = pass_new(d, 2, err, sizeof(err));
        CHECK(p != NULL, "P-0 %ux%u -> %ux%u: %s", sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3], err);
        if (p)
            printf("P-0 %ux%u -> %ux%u: eight pipelines, %.1f MB of images\n", sizes[i][0], sizes[i][1], sizes[i][2],
                   sizes[i][3], fsr3_pass_memory_bytes(p) / 1048576.0);
        fsr3_pass_destroy(p);
    }
    CHECK(s_messages == before, "P-0: %u validation messages", s_messages - before);

    char err[160] = "";
    Fsr3HostDesc d = desc_for(960, 544);
    d.flags &= ~(uint32_t)FSR3_HOST_ENABLE_DEPTH_INVERTED;
    Fsr3Pass *p = pass_new(d, 1, err, sizeof(err));
    CHECK(p == NULL && strstr(err, "inverted depth"), "P-0: forward depth accepted (%s)", err);
    fsr3_pass_destroy(p);
    d = desc_for(960, 544);
    d.max_render_width = 1;
    p = pass_new(d, 1, err, sizeof(err));
    CHECK(p == NULL, "P-0: a 1-pixel-wide maximum render size accepted");
    fsr3_pass_destroy(p);
}

/* ---- F-S ------------------------------------------------------------------------------------- */

/* The bars in this file were measured on an RTX 3080 and on lavapipe (the values, and every mutant's,
 * are in the pull request that added them); each sits between the unmutated value and the nearest
 * killed mutant's on both. FSR's error over the baseline's on the quad after 32 frames: */
#define FS_RATIO_BAR_2X 0.75 /* 3080: 0.608; jitter dropped 0.997, x sign 1.013, y sign 0.877 */
#define FS_RATIO_BAR_1X 0.60 /* 3080: 0.309; jitter dropped 0.906 (the sign mutants stay at 0.35-0.39) */
/* Render pixels between the raster's measured shift and the jitter FSR is told. The 512/480 mutant
 * is 0.034 px off, and moves FSR's error by only 1-4%, below the error bars' reach. */
#define FS_SHIFT_BAR 0.010

static int on_lines(const Run *r, const Scene *s, int t, double x, double y) {
    (void)r;
    (void)t;
    return x >= s->q[0].x0 + 2 && x < s->q[0].x1 - 2 && y >= s->q[0].y0 + 2 && y < s->q[0].y1 - 2;
}

/* How far the raster moved the scene: a near-vertical and a near-horizontal edge, each crossing
 * one pixel over the canvas, so the count of covered pixels measures the shift to 1/272 px. */
static void measured_shift(const Run *r, double *dx, double *dy) {
    const double slope_x = 1.0 / CANVAS_H, slope_y = 1.0 / CANVAS_W;
    long nx = 0, ny = 0, nx0 = 0, ny0 = 0;
    for (int y = 0; y < CANVAS_H; y++)
        for (int x = 0; x < CANVAS_W; x++) {
            const double px = x + 0.5 - r->sx, py = y + 0.5 - r->sy;
            nx += px < 256.0 + slope_x * py;
            ny += py < 136.0 + slope_y * px;
            nx0 += x + 0.5 < 256.0 + slope_x * (y + 0.5);
            ny0 += y + 0.5 < 136.0 + slope_y * (x + 0.5);
        }
    *dx = (double)(nx - nx0) / CANVAS_H;
    *dy = (double)(ny - ny0) / CANVAS_W;
}

static void cell_fs(void) {
    Scene s;
    memset(&s, 0, sizeof(s));
    s.bg_tex = TEX_GREY;
    s.bg_w = 16;
    s.nq = 1;
    s.q[0] = (Quad){ 40, 30, 440, 242, 0, 0, 4, TEX_LINES, 1, 0, 0, 0 };
    for (int ratio = 2; ratio >= 1; ratio--) {
        Run r;
        if (!run_open(&r, RENDER_W * ratio, RENDER_H * ratio)) { run_close(&r); return; }
        double worst_shift = 0;
        for (int t = 0; t < 32; t++) {
            if (!frame(&r, &s, 0, t == 0)) break;
            double dx, dy;
            measured_shift(&r, &dx, &dy);
            worst_shift = fmax(worst_shift, fmax(fabs(dx - r.jx), fabs(dy - r.jy)));
        }
        truth(&r, &s, 0);
        baseline(&r, &s, 0);
        const double e = mae(&r, NULL, &s, 0, on_lines, NULL), b = mae(&r, r.base, &s, 0, on_lines, NULL);
        const double bar = ratio == 2 ? FS_RATIO_BAR_2X : FS_RATIO_BAR_1X;
        printf("F-S ratio %d: error %.2f levels, baseline %.2f, ratio %.3f (bar %.2f); raster shift off the jitter by %.4f px (bar %.3f)\n",
               ratio, e, b, e / b, bar, worst_shift, FS_SHIFT_BAR);
        CHECK(worst_shift <= FS_SHIFT_BAR, "F-S ratio %d: the raster's shift is %.4f px off the jitter", ratio, worst_shift);
        CHECK(e <= bar * b, "F-S ratio %d: error %.2f not below %.2f x the baseline's %.2f", ratio, e, bar, b);
        run_close(&r);
    }
}

/* ---- F-M ------------------------------------------------------------------------------------- */

/* Levels, in the band the quad left over its last 3 frames. 3080: 6.07; vector sign 23.7, y 17.6, UV 30.0. */
#define FM_TRAIL_BAR 9.0

static int trailing(const Run *r, const Scene *s, int t, double x, double y) {
    (void)r;
    const Quad *q = &s->q[0];
    if (inside(q, x, y, t)) return 0;
    for (int k = 1; k <= 3; k++)
        if (inside(q, x, y, t - k)) return 1;
    return 0;
}

static void cell_fm(void) {
    Scene s;
    memset(&s, 0, sizeof(s));
    s.bg_tex = TEX_GREEN;
    s.bg_w = 8;
    s.nq = 1;
    s.q[0] = (Quad){ 60, 50, 180, 130, 3.5, 3.5, 4, TEX_RED, 1, 0, 0, 0 };
    Run r;
    if (!run_open(&r, RENDER_W * 2, RENDER_H * 2)) { run_close(&r); return; }
    const int last = 31;
    for (int t = 0; t <= last; t++)
        if (!frame(&r, &s, t, t == 0)) break;
    truth(&r, &s, last);
    unsigned n;
    const double trail = mae(&r, NULL, &s, last, trailing, &n), all = mae(&r, NULL, &s, last, NULL, NULL);
    printf("F-M: trailing band %.2f levels over %u pixels (bar %.1f); whole frame %.2f\n", trail, n, FM_TRAIL_BAR, all);
    CHECK(trail <= FM_TRAIL_BAR, "F-M: trailing-edge error %.2f above %.1f", trail, FM_TRAIL_BAR);
    run_close(&r);
}

/* ---- F-R ------------------------------------------------------------------------------------- */

/* Measured on an RTX 3080 over one phase cycle: where the edges passed, the worst frame is 13.7
 * levels and 23.1 with the reactive input unbound; on the side it left, the mean is 10.8 and 18.6.
 * Each bar sits near the geometric mean of the two. The unmutated error is not 0, since both bands
 * are edge pixels; the bars judge the streak the mutant adds. */
#define FR_STREAK_BAR 18.0 /* levels, the worst frame of a phase cycle, where its edges passed */
#define FR_BEHIND_BAR 14.0 /* levels, the mean over a phase cycle on the side it left */

/* The pixels the translucent quad's edges crossed in its last 2 frames, on either side. A blended
 * draw writes no vector (design 2.3), so FSR sees the surface under it, and only the reactive
 * input tells it the colour there does not follow those vectors. */
static int swept(const Run *r, const Scene *s, int t, double x, double y) {
    (void)r;
    return inside(&s->q[1], x, y, t) != inside(&s->q[1], x, y, t - 2);
}

/* The side it left: where it was 2 frames ago and is no longer. Keeping 2 pixels off its edges
 * (tried on the RTX 3080) separates the mutant less, 8.8 against 12.8, because the streak is
 * strongest right behind the edge. */
static int left_behind(const Run *r, const Scene *s, int t, double x, double y) {
    (void)r;
    return !inside(&s->q[1], x, y, t) && inside(&s->q[1], x, y, t - 2);
}

static void cell_fr(void) {
    Scene s;
    memset(&s, 0, sizeof(s));
    s.bg_tex = TEX_GREEN;
    s.bg_w = 8;
    s.nq = 2;
    s.q[0] = (Quad){ 20, 60, 220, 210, 2.5, 0, 4, TEX_RED, 1, 0, 0, 0 };
    s.q[1] = (Quad){ 300, 90, 400, 180, -4, 0, 2, TEX_BLUE, 0.8, 0, 0, 0 };
    Run r;
    if (!run_open(&r, RENDER_W * 2, RENDER_H * 2)) { run_close(&r); return; }
    const int cycle = fsr3_jitter_phase_count(RENDER_W, r.dw);
    double worst = 0, behind_worst = 0, behind_sum = 0;
    int worst_t = 0, frames = 0;
    for (int t = 0; t < 16 + cycle; t++) {
        if (!frame(&r, &s, t, t == 0)) break;
        if (t < 16) continue;
        truth(&r, &s, t);
        const double e = mae(&r, NULL, &s, t, swept, NULL);
        const double b = mae(&r, NULL, &s, t, left_behind, NULL);
        if (e > worst) { worst = e; worst_t = t; }
        if (b > behind_worst) behind_worst = b;
        behind_sum += b;
        frames++;
    }
    const double behind_mean = frames ? behind_sum / frames : 0;
    printf("F-R: worst error where the translucent quad's edges passed %.2f levels at frame %d over a %d-frame phase cycle (bar %.1f); "
           "the side it left: mean %.2f, worst %.2f (bar %.1f)\n",
           worst, worst_t, cycle, FR_STREAK_BAR, behind_mean, behind_worst, FR_BEHIND_BAR);
    CHECK(worst <= FR_STREAK_BAR, "F-R: a streak of %.2f levels at the translucent quad's edges (frame %d)", worst,
          worst_t);
    CHECK(behind_mean <= FR_BEHIND_BAR, "F-R: a streak of %.2f levels on the side the translucent quad left", behind_mean);
    run_close(&r);
}

/* ---- F-T ------------------------------------------------------------------------------------- */

/* Measured on an RTX 3080: the ghost is 15.8 levels with T&C 0.9 and 30.3 with the T&C input
 * ignored; the static region changes 0.8 levels a frame, and 18.1 when T&C covers the whole frame.
 * In FSR 3.1.6 the reactive input joins the shading-change mask, which also resets the history's
 * accumulation (ffx_fsr3upscaler_prepare_reactivity.h:220, 262), while T&C only joins the mask that
 * widens the colour clamp and weakens locks (line 266). So even at 0.9 the ghost stays about twice
 * what the same value gives on the reactive input (7.7, recorded by the mutants). */
#define FT_GHOST_BAR 22.0  /* levels, on and behind the quad whose vectors are wrongly 0 */
#define FT_STATIC_BAR 4.0  /* levels, a static region far from it */

static int ghost_region(const Run *r, const Scene *s, int t, double x, double y) {
    return inside(&s->q[0], x, y, t) || trailing(r, s, t, x, y);
}

/* The still, thin-lined quad: where history is what keeps the lines from flickering. */
static int static_region(const Run *r, const Scene *s, int t, double x, double y) {
    (void)r;
    (void)t;
    const Quad *q = &s->q[1];
    return x >= q->x0 + 2 && x < q->x1 - 2 && y >= q->y0 + 2 && y < q->y1 - 2;
}

static int on_scroll(const Run *r, const Scene *s, int t, double x, double y) {
    (void)r;
    (void)t;
    const Quad *q = &s->q[0];
    return x >= q->x0 + 2 && x < q->x1 - 2 && y >= q->y0 + 2 && y < q->y1 - 2;
}

static void cell_ft(void) {
    Scene s;
    memset(&s, 0, sizeof(s));
    s.bg_tex = TEX_GREEN;
    s.bg_w = 8;
    s.nq = 2;
    s.q[0] = (Quad){ 40, 40, 140, 120, 3, 0, 4, TEX_RED, 1, 0.9, 1, 0 };
    s.q[1] = (Quad){ 300, 150, 460, 260, 0, 0, 6, TEX_LINES, 1, 0, 0, 0 };
    Run r;
    if (!run_open(&r, RENDER_W * 2, RENDER_H * 2)) { run_close(&r); return; }
    const int last = 31;
    uint8_t *prev = malloc((size_t)r.dw * r.dh * 4);
    if (!prev) { run_close(&r); return; }
    double flicker = 0;
    unsigned flicker_frames = 0;
    for (int t = 0; t <= last; t++) {
        if (!frame(&r, &s, t, t == 0)) break;
        if (t > last - 8) { /* the static region's change from frame to frame, over the last 8 */
            double sum = 0;
            unsigned n = 0;
            for (int v = 0; v < r.dh; v++)
                for (int u = 0; u < r.dw; u++) {
                    if (!static_region(&r, &s, t, (u + 0.5) / 2, (v + 0.5) / 2)) continue;
                    const size_t i = ((size_t)v * r.dw + u) * 4;
                    for (int c = 0; c < 3; c++) sum += abs((int)r.out_px[i + c] - (int)prev[i + c]);
                    n++;
                }
            flicker += sum / (3.0 * n);
            flicker_frames++;
        }
        memcpy(prev, r.out_px, (size_t)r.dw * r.dh * 4);
    }
    free(prev);
    truth(&r, &s, last);
    const double ghost = mae(&r, NULL, &s, last, ghost_region, NULL);
    const double still = flicker_frames ? flicker / flicker_frames : 0;
    printf("F-T: ghost %.2f levels (bar %.1f); static region changes %.2f levels a frame (bar %.1f)\n", ghost,
           FT_GHOST_BAR, still, FT_STATIC_BAR);
    CHECK(ghost <= FT_GHOST_BAR, "F-T: a ghost of %.2f levels where the vector is wrongly 0", ghost);
    CHECK(still <= FT_STATIC_BAR, "F-T: the static region changes %.2f levels a frame, above %.1f", still,
          FT_STATIC_BAR);
    run_close(&r);

    /* The grades for a texture that scrolls on still geometry: recorded, not judged (design 2.5). */
    static const double grades[] = { 0.0, 0.25, 0.5, 0.75, 0.9 };
    for (unsigned i = 0; i < sizeof(grades) / sizeof(grades[0]); i++) {
        Scene sc;
        memset(&sc, 0, sizeof(sc));
        sc.bg_tex = TEX_GREY;
        sc.bg_w = 8;
        sc.nq = 1;
        sc.q[0] = (Quad){ 100, 60, 380, 220, 0, 0, 4, TEX_BLUE, 1, grades[i], 0, 1.5 };
        if (!run_open(&r, RENDER_W * 2, RENDER_H * 2)) { run_close(&r); return; }
        for (int t = 0; t <= 23; t++)
            if (!frame(&r, &sc, t, t == 0)) break;
        truth(&r, &sc, 23);
        printf("F-T grade %.2f on a scrolling texture: %.2f levels\n", grades[i], mae(&r, NULL, &sc, 23, on_scroll, NULL));
        run_close(&r);
    }
}

/* ---- F-C ------------------------------------------------------------------------------------- */

/* The first frame after the cut, over the baseline's error. 3080: 1.018; reset ignored 2.86, its clears
 * skipped 3.41. */
#define FC_RATIO_BAR 1.50
/* The old scene's share of it. 3080: 0.021 (FSR's own difference from the bilinear baseline);
 * reset ignored 0.368, its clears skipped 0.311. */
#define FC_OLD_BAR 0.10

static void cell_fc(void) {
    Scene a, b;
    memset(&a, 0, sizeof(a));
    a.bg_tex = TEX_GREEN;
    a.bg_w = 8;
    a.nq = 1;
    a.q[0] = (Quad){ 60, 40, 260, 200, 0, 0, 4, TEX_RED, 1, 0, 0, 0 };
    b = a;
    b.bg_tex = TEX_BLUE;
    b.q[0] = (Quad){ 220, 90, 440, 250, 0, 0, 4, TEX_LINES, 1, 0, 0, 0 };
    Run r;
    if (!run_open(&r, RENDER_W * 2, RENDER_H * 2)) { run_close(&r); return; }
    for (int t = 0; t < 16; t++)
        if (!frame(&r, &a, t, t == 0)) break;
    float *ta = malloc((size_t)r.dw * r.dh * 3 * sizeof(float));
    if (!ta) { run_close(&r); return; }
    truth(&r, &a, 15);
    memcpy(ta, r.truth, (size_t)r.dw * r.dh * 3 * sizeof(float));
    frame(&r, &b, 16, 1);
    truth(&r, &b, 16);
    baseline(&r, &b, 16);
    const double e = mae(&r, NULL, &b, 16, NULL, NULL), base = mae(&r, r.base, &b, 16, NULL, NULL);
    /* The old scene's share of the output: out - B' projected on A - B', where B' is the spatial
     * upscale of the new scene, so 0 holds nothing of A and 1 is A. */
    double num = 0, den = 0;
    for (size_t i = 0; i < (size_t)r.dw * r.dh * 3; i++) {
        const double d = ta[i] - r.base[i];
        num += (r.out_px[i / 3 * 4 + i % 3] / 255.0 - r.base[i]) * d;
        den += d * d;
    }
    const double share = den > 0 ? num / den : 0.0;
    printf("F-C: first frame after the cut %.2f levels, baseline %.2f, ratio %.3f (bar %.2f); the old scene's share %.4f (bar %.3f)\n",
           e, base, e / base, FC_RATIO_BAR, share, FC_OLD_BAR);
    CHECK(e <= FC_RATIO_BAR * base, "F-C: the first frame after the cut is %.2f, above %.2f x the baseline's %.2f", e,
          FC_RATIO_BAR, base);
    CHECK(fabs(share) <= FC_OLD_BAR, "F-C: the old scene's share of the first frame after the cut is %.4f", share);
    free(ta);
    run_close(&r);
}

/* ---- F-D ------------------------------------------------------------------------------------- */

/* Levels, in the band the near quad uncovered over its last 3 frames. 3080: 6.23 under every mapping;
 * window z in place of k/w 10.04 (full) and 10.28 (partial), 6.23 (inverted, where window z happens to
 * order as inverted depth). */
#define FD_UNCOVER_BAR 8.0

static void cell_fd(void) {
    static const char *names[] = { "full", "partial", "inverted" };
    for (int zmap = ZMAP_FULL; zmap <= ZMAP_INVERTED; zmap++) {
        Scene s;
        memset(&s, 0, sizeof(s));
        s.bg_tex = TEX_BLUE;
        s.bg_w = 16;
        s.zmap = zmap;
        s.nq = 2;
        s.q[0] = (Quad){ 80, 50, 200, 170, 3, 0, 2, TEX_RED, 1, 0, 0, 0 };   /* near, moving */
        s.q[1] = (Quad){ 40, 30, 440, 240, 0, 0, 8, TEX_GREEN, 1, 0, 0, 0 }; /* far, still */
        Run r;
        if (!run_open(&r, RENDER_W * 2, RENDER_H * 2)) { run_close(&r); return; }
        const int last = 31;
        for (int t = 0; t <= last; t++)
            if (!frame(&r, &s, t, t == 0)) break;
        truth(&r, &s, last);
        unsigned n;
        const double e = mae(&r, NULL, &s, last, trailing, &n);
        printf("F-D %s z mapping: uncovered band %.2f levels over %u pixels (bar %.1f)\n", names[zmap], e, n,
               FD_UNCOVER_BAR);
        CHECK(e <= FD_UNCOVER_BAR, "F-D %s: the uncovered pixels are %.2f levels off the far quad", names[zmap], e);
        run_close(&r);
    }
}

/* ---- main ------------------------------------------------------------------------------------ */

static const struct {
    const char *name;
    void (*run)(void);
} k_cells[] = {
    { "P-0", cell_p0 }, { "F-S", cell_fs }, { "F-M", cell_fm }, { "F-R", cell_fr },
    { "F-T", cell_ft }, { "F-C", cell_fc }, { "F-D", cell_fd },
};
#define CELL_COUNT (sizeof(k_cells) / sizeof(k_cells[0]))

int main(int argc, char **argv) {
    int want[CELL_COUNT], any = 0;
    memset(want, 0, sizeof(want));
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--cell") && a + 1 < argc) {
            int found = 0;
            for (unsigned c = 0; c < CELL_COUNT; c++)
                if (!strcmp(argv[a + 1], k_cells[c].name)) want[c] = found = any = 1;
            if (!found) { fprintf(stderr, "unknown cell %s\n", argv[a + 1]); return 2; }
            a++;
        } else {
            fprintf(stderr, "usage: fsr3_pass_selftest [--cell NAME]...\n");
            return 2;
        }
    }
    char why[160] = "";
    const int up = gpu_open(why, sizeof(why));
    if (up != 1) {
        printf("SKIP fsr3-pass-selftest: %s\n", up == 0 ? why : "a Vulkan object could not be made");
        gpu_close();
        return up == 0 ? 77 : 1;
    }
    syncval_canary();
    shader_canary();
    if (g.validation && !g_syncval_shader) {
        /* The cells still run, but nothing checks the barriers between passes. */
        printf("SKIP the barrier proof (synchronisation validation of shader accesses is not active)\n");
        s_skip = 1;
    }
    for (unsigned c = 0; c < CELL_COUNT; c++)
        if (!any || want[c]) {
            const unsigned before = s_messages;
            k_cells[c].run();
            CHECK(s_messages == before, "%s: %u validation messages", k_cells[c].name, s_messages - before);
        }
    gpu_close();
    if (s_fail) return 1;
    if (s_skip) return 77;
    printf("fsr3-pass-selftest: ok\n");
    return 0;
}
