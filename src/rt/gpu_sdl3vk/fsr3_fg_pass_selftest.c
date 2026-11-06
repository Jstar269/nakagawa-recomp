/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr3_fg_pass_selftest.c - FSR 3's frame generation (fsr3_fg_pass.c) on a headless Vulkan device,
 * under the validation layers with synchronisation validation when they are installed. Links the
 * pass, its host and the C library alone.
 *
 *   G-R  the device refusal: fsr3_fg_pass_refusal on a table of device facts, one gap at a time; then
 *        on this device, a device it refuses (its own facts, or this device's facts with the subgroup
 *        size forced out of reach, or without the features enabled) makes fsr3_fg_pass_create return
 *        its one reason without a single Vulkan call
 *   G-0  the seventeen pipelines at six size pairs with no validation message, the refused sizes, and
 *        whole frames recorded and run (prepare, configure, dispatch; with and without a HUD-less
 *        colour; a reset) with no validation message, the refused calls leaving the context as it was
 *
 * usage: fsr3_fg_pass_selftest [--cell NAME]...    (default: every cell)
 * Exit 77 (SKIP) without a Vulkan loader or device or, for G-0, without the validation layer or on a
 * device the pass refuses (G-R still runs there: that refusal is what it checks).
 */
#include "fsr3_fg_pass.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#define RENDER_W 480
#define RENDER_H 272
#define DISPLAY_W 960
#define DISPLAY_H 544
#define COLOR_FORMAT VK_FORMAT_R8G8B8A8_UNORM

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
    Fsr3FgDeviceFacts facts;
    const char *refusal; /* fsr3_fg_pass_refusal of this device's facts */
    int features_enabled;
} Gpu;
static Gpu g;
static unsigned s_messages; /* validation messages so far */

#define GPU_FN(name) static PFN_vk##name name
GPU_FN(CreateInstance); GPU_FN(DestroyInstance); GPU_FN(EnumerateInstanceLayerProperties);
GPU_FN(EnumerateInstanceExtensionProperties); GPU_FN(EnumerateInstanceVersion); GPU_FN(EnumeratePhysicalDevices);
GPU_FN(GetPhysicalDeviceQueueFamilyProperties); GPU_FN(GetPhysicalDeviceMemoryProperties);
GPU_FN(GetPhysicalDeviceProperties);
GPU_FN(CreateDevice); GPU_FN(DestroyDevice); GPU_FN(GetDeviceProcAddr); GPU_FN(GetDeviceQueue);
GPU_FN(CreateCommandPool); GPU_FN(DestroyCommandPool); GPU_FN(AllocateCommandBuffers);
GPU_FN(CreateFence); GPU_FN(DestroyFence); GPU_FN(WaitForFences); GPU_FN(ResetFences);
GPU_FN(CreateImage); GPU_FN(DestroyImage); GPU_FN(GetImageMemoryRequirements);
GPU_FN(CreateImageView); GPU_FN(DestroyImageView);
GPU_FN(AllocateMemory); GPU_FN(FreeMemory); GPU_FN(BindImageMemory);
GPU_FN(BeginCommandBuffer); GPU_FN(EndCommandBuffer); GPU_FN(ResetCommandBuffer);
GPU_FN(CmdPipelineBarrier); GPU_FN(CmdClearColorImage);
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

/* 1, or 0 with the reason for a skip. A device the pass refuses is opened too: G-R runs on it. */
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
    EnumerateInstanceVersion = (PFN_vkEnumerateInstanceVersion)g.gipa(VK_NULL_HANDLE, "vkEnumerateInstanceVersion");
    if (!CreateInstance || !EnumerateInstanceExtensionProperties) { snprintf(why, cap, "no vkCreateInstance"); return 0; }
    uint32_t loader = VK_API_VERSION_1_0;
    if (!EnumerateInstanceVersion || EnumerateInstanceVersion(&loader) != VK_SUCCESS || loader < VK_API_VERSION_1_1) {
        snprintf(why, cap, "a Vulkan 1.0 loader");
        return 0;
    }

    const char *layer = "VK_LAYER_KHRONOS_validation";
    g.validation = has_layer(layer) && has_extension(NULL, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    const int sync = g.validation && has_extension(layer, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    const int settings = sync && has_extension(layer, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
    const char *extensions[3] = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME,
                                  VK_EXT_LAYER_SETTINGS_EXTENSION_NAME };
    /* Without the shader-access heuristic, synchronisation validation does not see a dispatch's reads
     * and writes through its descriptors (fsr3_pass_selftest.c measured it on SDK 1.4.363). */
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

    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "fsr3_fg_pass_selftest", 1, NULL, 0,
                              loader < VK_API_VERSION_1_3 ? loader : VK_API_VERSION_1_3 };
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
                                              "checked, and the canary measures it)"
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
    fsr3_fg_pass_query(g.gipa, g.instance, g.physical, COLOR_FORMAT, &g.facts);
    g.refusal = fsr3_fg_pass_refusal(&g.facts);
    printf("device: %s (driver 0x%x, Vulkan %u.%u), subgroup sizes %u to %u%s: %s%s\n", props.deviceName,
           props.driverVersion, VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion),
           g.facts.min_subgroup_size, g.facts.max_subgroup_size,
           (g.facts.required_size_stages & VK_SHADER_STAGE_COMPUTE_BIT) ? " (required sizes in compute)" : "",
           g.refusal ? "refused, " : "accepted", g.refusal ? g.refusal : "");

    /* The features the pass needs, those this device has. */
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g.family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkPhysicalDeviceSubgroupSizeControlFeatures sf;
    memset(&sf, 0, sizeof(sf));
    sf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES;
    sf.subgroupSizeControl = g.facts.size_control ? VK_TRUE : VK_FALSE;
    sf.computeFullSubgroups = g.facts.full_subgroups ? VK_TRUE : VK_FALSE;
    const int core13 = g.facts.api_version >= VK_API_VERSION_1_3 && app.apiVersion >= VK_API_VERSION_1_3;
    const int size_control = g.facts.size_control_extension || core13;
    VkPhysicalDeviceFeatures2 f2;
    memset(&f2, 0, sizeof(f2));
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = size_control && (sf.subgroupSizeControl || sf.computeFullSubgroups) ? &sf : NULL;
    f2.features.shaderStorageImageReadWithoutFormat = g.facts.storage_read_without_format ? VK_TRUE : VK_FALSE;
    f2.features.shaderStorageImageWriteWithoutFormat = g.facts.storage_write_without_format ? VK_TRUE : VK_FALSE;
    const char *ext = VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME;
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    if (g.facts.size_control_extension) {
        dci.enabledExtensionCount = 1;
        dci.ppEnabledExtensionNames = &ext;
    }
    if (CreateDevice(g.physical, &dci, NULL, &g.device) != VK_SUCCESS) {
        snprintf(why, cap, "vkCreateDevice failed");
        return 0;
    }
    g.features_enabled = !g.refusal && sf.subgroupSizeControl && sf.computeFullSubgroups && f2.pNext;
#define DEV(name) name = (PFN_vk##name)GetDeviceProcAddr(g.device, "vk" #name)
    DEV(DestroyDevice); DEV(GetDeviceQueue); DEV(CreateCommandPool); DEV(DestroyCommandPool);
    DEV(AllocateCommandBuffers); DEV(CreateFence); DEV(DestroyFence); DEV(WaitForFences);
    DEV(ResetFences); DEV(CreateImage); DEV(DestroyImage); DEV(GetImageMemoryRequirements);
    DEV(CreateImageView); DEV(DestroyImageView); DEV(AllocateMemory); DEV(FreeMemory); DEV(BindImageMemory);
    DEV(BeginCommandBuffer); DEV(EndCommandBuffer); DEV(ResetCommandBuffer); DEV(CmdPipelineBarrier);
    DEV(CmdClearColorImage); DEV(QueueSubmit); DEV(DeviceWaitIdle);
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
    return 1;
}

static void gpu_close(void) {
    if (g.device) {
        DeviceWaitIdle(g.device);
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
    if (usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        VkClearColorValue v;
        memset(&v, 0, sizeof(v));
        CmdClearColorImage(g.cb, t->image, VK_IMAGE_LAYOUT_GENERAL, &v, 1, &vi.subresourceRange);
    }
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
 * assumed: two clears of one image with no barrier between them are a write-after-write hazard. The
 * dispatch canary below measures that it also sees shader accesses. */
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

/* ---- the pass -------------------------------------------------------------------------------- */

static unsigned s_gipa_calls; /* Vulkan functions the pass looked up through counting_gipa */
static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL counting_gipa(VkInstance instance, const char *name) {
    s_gipa_calls++;
    return g.gipa(instance, name);
}

static Fsr3FgHostDesc desc_for(uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh) {
    Fsr3FgHostDesc d;
    memset(&d, 0, sizeof(d));
    d.flags = FSR3FG_HOST_ENABLE_DEPTH_INVERTED | FSR3FG_HOST_ENABLE_DEPTH_INFINITE;
    d.max_render_width = rw;
    d.max_render_height = rh;
    d.display_width = dw;
    d.display_height = dh;
    return d;
}

static Fsr3FgPass *pass_new(Fsr3FgHostDesc desc, uint32_t frames, const Fsr3FgDeviceFacts *facts, int enabled,
                            char *err, size_t cap) {
    Fsr3FgPassInit init;
    memset(&init, 0, sizeof(init));
    init.get_instance_proc_addr = counting_gipa;
    init.instance = g.instance;
    init.physical_device = g.physical;
    init.device = g.device;
    init.frames_in_flight = frames;
    init.desc = desc;
    init.color_format = COLOR_FORMAT;
    init.facts = facts;
    init.features_enabled = enabled;
    return fsr3_fg_pass_create(&init, err, cap);
}

/* ---- G-R ------------------------------------------------------------------------------------- */

/* A device that has everything, as the facts describe it. */
static Fsr3FgDeviceFacts good_facts(void) {
    Fsr3FgDeviceFacts f;
    memset(&f, 0, sizeof(f));
    f.api_version = VK_API_VERSION_1_3;
    f.storage_read_without_format = f.storage_write_without_format = 1;
    f.subgroup_stages = VK_SHADER_STAGE_COMPUTE_BIT;
    f.subgroup_operations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_ARITHMETIC_BIT;
    f.size_control_extension = 1;
    f.size_control = f.full_subgroups = 1;
    f.min_subgroup_size = 32;
    f.max_subgroup_size = 32;
    f.required_size_stages = VK_SHADER_STAGE_COMPUTE_BIT;
    f.max_workgroup_invocations = 1024;
    f.max_workgroup_subgroups = 32;
    for (int k = 0; k < 5; k++) f.max_per_stage[k] = 64;
    f.max_per_stage_resources = 128;
    return f;
}

static void cell_gr(void) {
    /* The table: each fact alone, against the reason it must give. */
    static const struct {
        const char *name;
        int field;
        const char *want; /* a substring of the reason; NULL: accepted */
    } rows[] = {
        { "everything", 0, NULL },
        { "Vulkan 1.0", 1, "Vulkan 1.1" },
        { "no read without format", 2, "ReadWithoutFormat" },
        { "no write without format", 3, "WriteWithoutFormat" },
        { "no subgroups in compute", 4, "subgroup arithmetic" },
        { "no subgroup arithmetic", 5, "subgroup arithmetic" },
        { "Vulkan 1.2 without the extension", 6, "subgroup size control" },
        { "Vulkan 1.3 without the extension", 7, NULL },
        { "no subgroupSizeControl", 8, "subgroupSizeControl" },
        { "no computeFullSubgroups", 9, "computeFullSubgroups" },
        { "subgroups of 64 only", 10, "required subgroup size of 32" },
        { "subgroups of 4 to 16", 11, "required subgroup size of 32" },
        { "subgroups of 8 to 64", 12, NULL },
        { "required sizes not in compute", 13, "not in compute" },
        { "128 invocations a workgroup", 14, "256 invocations" },
        { "4 storage images a stage", 15, "storage images" },
        { "a format gap", 16, "format 98" },
        { "one subgroup a workgroup", 17, "two 32-lane subgroups" },
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        Fsr3FgDeviceFacts f = good_facts();
        switch (rows[i].field) {
        case 1: f.api_version = VK_API_VERSION_1_0; break;
        case 2: f.storage_read_without_format = 0; break;
        case 3: f.storage_write_without_format = 0; break;
        case 4: f.subgroup_stages = VK_SHADER_STAGE_FRAGMENT_BIT; break;
        case 5: f.subgroup_operations = VK_SUBGROUP_FEATURE_BASIC_BIT | VK_SUBGROUP_FEATURE_BALLOT_BIT; break;
        case 6: f.api_version = VK_API_VERSION_1_2; f.size_control_extension = 0; break;
        case 7: f.size_control_extension = 0; break;
        case 8: f.size_control = 0; break;
        case 9: f.full_subgroups = 0; break;
        case 10: f.min_subgroup_size = f.max_subgroup_size = 64; break;
        case 11: f.min_subgroup_size = 4; f.max_subgroup_size = 16; break;
        case 12: f.min_subgroup_size = 8; f.max_subgroup_size = 64; break;
        case 13: f.required_size_stages = VK_SHADER_STAGE_FRAGMENT_BIT; break;
        case 14: f.max_workgroup_invocations = 128; break;
        case 15: f.max_per_stage[3] = 4; break;
        case 16: snprintf(f.format_missing, sizeof(f.format_missing), "format 98 features 0x80"); break;
        case 17: f.max_workgroup_subgroups = 1; break;
        default: break;
        }
        const char *got = fsr3_fg_pass_refusal(&f);
        if (rows[i].want)
            CHECK(got && strstr(got, rows[i].want), "G-R %s: got \"%s\", want \"%s\"", rows[i].name, got ? got : "(accepted)",
                  rows[i].want);
        else
            CHECK(!got, "G-R %s: refused: %s", rows[i].name, got);
    }
    printf("G-R: %u device-fact rows\n", (unsigned)(sizeof(rows) / sizeof(rows[0])));

    if (!g.device) return;
    /* On this device: a refused device gets the one reason and no Vulkan call. */
    Fsr3FgDeviceFacts forced = g.facts;
    const char *label = "its own facts";
    if (!g.refusal) {
        forced.min_subgroup_size = 4;
        forced.max_subgroup_size = 16;
        label = "its facts with subgroups of 4 to 16";
    }
    const char *want = fsr3_fg_pass_refusal(&forced);
    char err[200] = "";
    s_gipa_calls = 0;
    Fsr3FgPass *p = pass_new(desc_for(RENDER_W, RENDER_H, DISPLAY_W, DISPLAY_H), 2, &forced, g.features_enabled, err,
                             sizeof(err));
    CHECK(p == NULL, "G-R %s: created", label);
    CHECK(want && !strncmp(err, FSR3FG_PASS_REFUSED_PREFIX, strlen(FSR3FG_PASS_REFUSED_PREFIX)) &&
              !strcmp(err + strlen(FSR3FG_PASS_REFUSED_PREFIX), want),
          "G-R %s: the reason is \"%s\"", label, err);
    CHECK(s_gipa_calls == 0, "G-R %s: %u Vulkan functions looked up", label, s_gipa_calls);
    CHECK(strchr(err, '\n') == NULL, "G-R %s: the reason is more than one line", label);
    printf("G-R on this device (%s): %s\n", label, err);
    fsr3_fg_pass_destroy(p);
    if (!g.refusal) {
        s_gipa_calls = 0;
        p = pass_new(desc_for(RENDER_W, RENDER_H, DISPLAY_W, DISPLAY_H), 2, &g.facts, 0, err, sizeof(err));
        CHECK(p == NULL && strstr(err, "without the features") && s_gipa_calls == 0,
              "G-R features not enabled: %s, %u lookups", p ? "created" : err, s_gipa_calls);
        fsr3_fg_pass_destroy(p);
    }
}

/* ---- G-0 ------------------------------------------------------------------------------------- */

typedef struct {
    Fsr3FgPass *pass;
    uint32_t rw, rh, dw, dh;
    Tex depth, mv, present, hudless, out;
} Run;

static int run_open(Run *r, uint32_t rw, uint32_t rh, uint32_t dw, uint32_t dh, uint32_t frames) {
    memset(r, 0, sizeof(*r));
    r->rw = rw;
    r->rh = rh;
    r->dw = dw;
    r->dh = dh;
    char err[200] = "";
    r->pass = pass_new(desc_for(rw, rh, dw, dh), frames, &g.facts, g.features_enabled, err, sizeof(err));
    if (!r->pass) {
        printf("FAIL fsr3_fg_pass_create %ux%u -> %ux%u: %s\n", rw, rh, dw, dh, err);
        s_fail = 1;
        return 0;
    }
    const VkImageUsageFlags in = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const VkImageUsageFlags color = in | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    return tex_make(&r->depth, rw, rh, VK_FORMAT_R32_SFLOAT, in) &&
           tex_make(&r->mv, rw, rh, VK_FORMAT_R16G16_SFLOAT, in) &&
           tex_make(&r->present, dw, dh, COLOR_FORMAT, color) &&
           tex_make(&r->hudless, dw, dh, COLOR_FORMAT, color) &&
           tex_make(&r->out, dw, dh, COLOR_FORMAT,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

static void run_close(Run *r) {
    fsr3_fg_pass_destroy(r->pass);
    tex_free(&r->depth);
    tex_free(&r->mv);
    tex_free(&r->present);
    tex_free(&r->hudless);
    tex_free(&r->out);
}

static Fsr3FgPassPrepare prepare_for(const Run *r, uint64_t id, int reset, uint32_t slot) {
    Fsr3FgPassPrepare f;
    memset(&f, 0, sizeof(f));
    f.depth = r->depth.view;
    f.motion_vectors = r->mv.view;
    f.input_layout = VK_IMAGE_LAYOUT_GENERAL;
    f.slot = slot;
    f.params.frame_id = id;
    f.params.render_width = r->rw;
    f.params.render_height = r->rh;
    f.params.motion_vector_scale_x = 1.0f;
    f.params.motion_vector_scale_y = 1.0f;
    f.params.frame_time_delta = 1000.0f / 30.0f;
    f.params.camera_near = 1.0f;
    f.params.camera_far = 1000.0f;
    f.params.camera_fov_angle_vertical = 1.0471976f;
    f.params.view_space_to_meters_factor = 1.0f;
    f.params.reset = reset;
    return f;
}

static Fsr3FgPassDispatch dispatch_for(const Run *r, uint64_t id, int hudless, uint32_t slot) {
    Fsr3FgPassDispatch f;
    memset(&f, 0, sizeof(f));
    f.present_image = r->present.image;
    f.present = r->present.view;
    if (hudless) {
        f.hudless_image = r->hudless.image;
        f.hudless = r->hudless.view;
    }
    f.output = r->out.view;
    f.slot = slot;
    f.params.frame_id = id;
    f.params.min_max_luminance[1] = 1.0f;
    return f;
}

/* One real frame: prepare, configure and, unless `generate` is 0, dispatch. 1 when both were accepted. */
static int real_frame(Run *r, uint64_t id, int hudless, int reset, int generate, uint32_t slot) {
    const Fsr3FgPassPrepare pf = prepare_for(r, id, reset, slot);
    const Fsr3FgPassDispatch df = dispatch_for(r, id, hudless, slot);
    cmd_begin();
    barrier_all(); /* the last frame's reads, before this frame's uses */
    const int a = fsr3_fg_pass_prepare(r->pass, g.cb, &pf);
    fsr3_fg_pass_configure(r->pass, id, hudless);
    const int b = generate ? fsr3_fg_pass_dispatch(r->pass, g.cb, &df) : 0;
    barrier_all();
    cmd_end();
    return a == 0 && b == 0;
}

static void cell_g0(void) {
    if (!g.validation) {
        printf("SKIP G-0 (no validation layer: pipeline creation is not checked)\n");
        s_skip = 1;
        return;
    }
    if (g.refusal) {
        printf("SKIP G-0 (the pass refuses this device: %s)\n", g.refusal);
        s_skip = 1;
        return;
    }
    static const uint32_t sizes[][4] = {
        { 480, 272, 960, 544 }, { 480, 272, 480, 272 }, { 480, 272, 1440, 816 },
        { 333, 187, 1000, 561 }, { 480, 272, 3811, 2143 }, { 2, 2, 64, 64 },
    };
    const unsigned before = s_messages;
    for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        char err[200] = "";
        Fsr3FgPass *p = pass_new(desc_for(sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3]), 2, &g.facts,
                                 g.features_enabled, err, sizeof(err));
        CHECK(p != NULL, "G-0 %ux%u -> %ux%u: %s", sizes[i][0], sizes[i][1], sizes[i][2], sizes[i][3], err);
        if (p)
            printf("G-0 %ux%u -> %ux%u: seventeen pipelines, %.1f MB of images\n", sizes[i][0], sizes[i][1],
                   sizes[i][2], sizes[i][3], fsr3_fg_pass_memory_bytes(p) / 1048576.0);
        fsr3_fg_pass_destroy(p);
    }
    CHECK(s_messages == before, "G-0: %u validation messages creating the pipelines", s_messages - before);

    /* Refused creations. */
    static const struct {
        const char *name;
        uint32_t flags, rw, rh, dw, dh;
        const char *want;
    } refused[] = {
        { "forward depth", FSR3FG_HOST_ENABLE_DEPTH_INFINITE, 480, 272, 960, 544, "inverted depth" },
        { "a display of 63 rows", FSR3FG_HOST_ENABLE_DEPTH_INVERTED, 480, 272, 960, 63, "64 x 64" },
        { "a render size of 0", FSR3FG_HOST_ENABLE_DEPTH_INVERTED, 0, 272, 960, 544, "sizes above 0" },
    };
    for (unsigned i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        char err[200] = "";
        Fsr3FgHostDesc d = desc_for(refused[i].rw, refused[i].rh, refused[i].dw, refused[i].dh);
        d.flags = refused[i].flags;
        Fsr3FgPass *p = pass_new(d, 1, &g.facts, g.features_enabled, err, sizeof(err));
        CHECK(p == NULL && strstr(err, refused[i].want), "G-0 %s: %s", refused[i].name, p ? "created" : err);
        fsr3_fg_pass_destroy(p);
    }

    /* Whole frames under validation: the first (a reset), steady frames on two slots, the HUD-less
     * colour attached and detached, a frame prepared and not generated, and a reset. */
    Run r;
    if (!run_open(&r, RENDER_W, RENDER_H, DISPLAY_W, DISPLAY_H, 2)) {
        run_close(&r);
        return;
    }
    const unsigned frames_before = s_messages;
    static const struct { int hudless, reset, generate; } seq[] = {
        { 0, 0, 1 }, { 0, 0, 1 }, { 0, 0, 1 }, { 1, 0, 1 }, { 1, 0, 1 }, { 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 0, 0, 1 },
    };
    int ok = 1;
    for (unsigned i = 0; i < sizeof(seq) / sizeof(seq[0]); i++)
        ok &= real_frame(&r, 1 + i, seq[i].hudless, seq[i].reset, seq[i].generate, i % 2);
    CHECK(ok, "G-0: a frame was refused");
    CHECK(s_messages == frames_before, "G-0: %u validation messages running %u frames", s_messages - frames_before,
          (unsigned)(sizeof(seq) / sizeof(seq[0])));
    printf("G-0 frames: %u real frames, the last plan %u jobs, %u stages\n", (unsigned)(sizeof(seq) / sizeof(seq[0])),
           fsr3_fg_pass_host(r.pass)->job_count, fsr3_fg_pass_host(r.pass)->stage_count);

    /* Refused calls leave the context as it was and record nothing. */
    Fsr3FgHost *before_host = malloc(sizeof(*before_host));
    if (before_host) {
        memcpy(before_host, fsr3_fg_pass_host(r.pass), sizeof(*before_host));
        const uint64_t id = 20;
        Fsr3FgPassPrepare pf = prepare_for(&r, id, 0, 0);
        Fsr3FgPassDispatch df = dispatch_for(&r, id, 0, 0);
        int rc[7];
        cmd_begin();
        pf.slot = 2;
        rc[0] = fsr3_fg_pass_prepare(r.pass, g.cb, &pf); /* a slot past frames_in_flight */
        pf.slot = 0;
        pf.depth = VK_NULL_HANDLE;
        rc[1] = fsr3_fg_pass_prepare(r.pass, g.cb, &pf); /* no depth */
        pf.depth = r.depth.view;
        pf.params.render_width = RENDER_W + 1;
        rc[2] = fsr3_fg_pass_prepare(r.pass, g.cb, &pf); /* above the maximum: the host refuses */
        pf.params.render_width = RENDER_W;
        pf.input_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        rc[3] = fsr3_fg_pass_prepare(r.pass, g.cb, &pf); /* a layout the shaders cannot read */
        df.hudless_image = r.hudless.image;
        df.hudless = r.hudless.view;
        rc[4] = fsr3_fg_pass_dispatch(r.pass, g.cb, &df); /* a HUD-less colour the configure did not announce */
        df.hudless_image = VK_NULL_HANDLE;
        df.hudless = VK_NULL_HANDLE;
        df.params.generation_rect[0] = DISPLAY_W - 8;
        df.params.generation_rect[2] = 16;
        df.params.generation_rect[3] = 16;
        rc[5] = fsr3_fg_pass_dispatch(r.pass, g.cb, &df); /* a rectangle past the display */
        df.params.generation_rect[0] = df.params.generation_rect[2] = df.params.generation_rect[3] = 0;
        df.output = VK_NULL_HANDLE;
        rc[6] = fsr3_fg_pass_dispatch(r.pass, g.cb, &df); /* no output */
        cmd_end();
        for (int k = 0; k < 7; k++) CHECK(rc[k] == -1, "G-0 refused call %d returned %d", k, rc[k]);
        CHECK(!memcmp(before_host, fsr3_fg_pass_host(r.pass), sizeof(*before_host)),
              "G-0: a refused call changed the context");
        free(before_host);
    }
    run_close(&r);
}

/* ---- main ------------------------------------------------------------------------------------ */

static const struct {
    const char *name;
    void (*run)(void);
} k_cells[] = {
    { "G-R", cell_gr }, { "G-0", cell_g0 },
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
            fprintf(stderr, "usage: fsr3_fg_pass_selftest [--cell NAME]...\n");
            return 2;
        }
    }
    char why[160] = "";
    const int up = gpu_open(why, sizeof(why));
    if (up != 1) {
        /* G-R's table needs no device. */
        if (!any || want[0]) cell_gr();
        printf("SKIP fsr3-fg-pass-selftest: %s\n", up == 0 ? why : "a Vulkan object could not be made");
        gpu_close();
        return s_fail || up != 0 ? 1 : 77;
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
    printf("fsr3-fg-pass-selftest: ok\n");
    return 0;
}
