/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* nk_ui Vulkan backend (lane C3c): draw lists rendered on a headless Vulkan
   device (no window) into an offscreen RGBA8 image, read back and checked pixel
   by pixel. The expected pixels are worked out from each list, as the SDL_Renderer
   backend's selftest checks its software output; the waves are compared with
   the CPU reference (nk_ui_waves_render_command) sampled the way a linear filter
   samples it.

   Exit 77 means SKIP, with the reason on stdout: the build lacked the Vulkan
   headers (NK_UI_WITH_VULKAN), or no Vulkan loader, or no device with Vulkan 1.1
   and a graphics queue, is present. The loader is opened at run time, so the
   test has no link-time Vulkan dependency, like the backend. When the Khronos
   validation layer is installed it is enabled, and any error it reports fails
   the run. No file is read. */

#if defined(NK_UI_WITH_VULKAN) && !defined(VK_NO_PROTOTYPES)
#define VK_NO_PROTOTYPES 1
#endif

#include "nk_ui.h"
#include "nk_ui_draw.h"
#include "nk_ui_image.h"
#include "nk_ui_waves.h"
#include "backend/nk_ui_vk.h"

#include <stdio.h>
#include <string.h>

#define EXIT_SKIP 77

#if !defined(NK_UI_WITH_VULKAN)

int main(void) {
    printf("SKIP nk_ui_vk: the Vulkan headers were not available at build time "
           "(NK_UI_WITH_VULKAN not defined)\n");
    return EXIT_SKIP;
}

#else

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <math.h>
#include <stdlib.h>

static int g_failures = 0;

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
            ++g_failures; \
            return 0; \
        } \
    } while (0)

/* ---- the headless device ---- */

#define GLOBAL_FUNCS(X) \
    X(CreateInstance) X(EnumerateInstanceVersion) X(EnumerateInstanceLayerProperties) \
    X(EnumerateInstanceExtensionProperties)

#define INSTANCE_FUNCS(X) \
    X(DestroyInstance) X(EnumeratePhysicalDevices) X(GetPhysicalDeviceProperties) \
    X(GetPhysicalDeviceQueueFamilyProperties) X(GetPhysicalDeviceMemoryProperties) \
    X(CreateDevice) X(GetDeviceProcAddr)

#define DEVICE_FUNCS(X) \
    X(DestroyDevice) X(GetDeviceQueue) X(DeviceWaitIdle) X(CreateCommandPool) \
    X(DestroyCommandPool) X(AllocateCommandBuffers) X(BeginCommandBuffer) X(EndCommandBuffer) \
    X(ResetCommandBuffer) X(QueueSubmit) X(CreateFence) X(DestroyFence) X(WaitForFences) \
    X(ResetFences) X(CreateRenderPass) X(DestroyRenderPass) X(CreateImage) X(DestroyImage) \
    X(GetImageMemoryRequirements) X(BindImageMemory) X(AllocateMemory) X(FreeMemory) \
    X(CreateImageView) X(DestroyImageView) X(CreateFramebuffer) X(DestroyFramebuffer) \
    X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(BindBufferMemory) \
    X(MapMemory) X(UnmapMemory) X(InvalidateMappedMemoryRanges) X(CmdBeginRenderPass) \
    X(CmdEndRenderPass) X(CmdCopyImageToBuffer) X(CmdPipelineBarrier)

typedef struct {
    void *library;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
#define FIELD(name) PFN_vk##name name;
    GLOBAL_FUNCS(FIELD)
    INSTANCE_FUNCS(FIELD)
    DEVICE_FUNCS(FIELD)
#undef FIELD
    PFN_vkCreateDebugUtilsMessengerEXT CreateDebugUtilsMessengerEXT;
    PFN_vkDestroyDebugUtilsMessengerEXT DestroyDebugUtilsMessengerEXT;
    VkInstance instance;
    VkDebugUtilsMessengerEXT messenger;
    VkPhysicalDevice physical;
    VkPhysicalDeviceMemoryProperties memory;
    VkDevice device;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    VkCommandBuffer cb;
    VkFence fence;
    VkRenderPass render_pass;
    char device_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    bool validation;
} Gpu;

static Gpu g_gpu;
static int g_validation_errors = 0;
static uint32_t g_frame = 0;

/* Frames in flight for every backend here; frames rotate through the slots, so
   resources retired by one frame are freed when its slot comes round again. */
#define FRAMES 2u

static VKAPI_ATTR VkBool32 VKAPI_CALL on_validation(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT types,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)types;
    (void)user;
    if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0u) {
        ++g_validation_errors;
        printf("VALIDATION ERROR: %s\n", data != NULL && data->pMessage != NULL ? data->pMessage : "?");
    } else if ((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0u) {
        printf("validation warning: %s\n", data != NULL && data->pMessage != NULL ? data->pMessage : "?");
    }
    return VK_FALSE;
}

static bool loader_open(void) {
    void *symbol = NULL;
#if defined(_WIN32)
    HMODULE module = LoadLibraryA("vulkan-1.dll");
    FARPROC proc;
    if (module == NULL) {
        return false;
    }
    proc = GetProcAddress(module, "vkGetInstanceProcAddr");
    g_gpu.library = (void *)module;
    memcpy(&symbol, &proc, sizeof(symbol));
#else
    void *library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (library == NULL) {
        library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (library == NULL) {
        return false;
    }
    g_gpu.library = library;
    symbol = dlsym(library, "vkGetInstanceProcAddr");
#endif
    if (symbol == NULL) {
        return false;
    }
    /* An object pointer to a function pointer, the size of both. */
    memcpy(&g_gpu.GetInstanceProcAddr, &symbol, sizeof(g_gpu.GetInstanceProcAddr));
    return true;
}

static void loader_close(void) {
    if (g_gpu.library == NULL) {
        return;
    }
#if defined(_WIN32)
    FreeLibrary((HMODULE)g_gpu.library);
#else
    dlclose(g_gpu.library);
#endif
    g_gpu.library = NULL;
}

static uint32_t memory_type(uint32_t bits, VkMemoryPropertyFlags want) {
    uint32_t i;
    for (i = 0; i < g_gpu.memory.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) != 0u && (g_gpu.memory.memoryTypes[i].propertyFlags & want) == want) {
            return i;
        }
    }
    return UINT32_MAX;
}

static bool has_layer(const char *name) {
    uint32_t count = 0;
    VkLayerProperties *layers;
    bool found = false;
    uint32_t i;
    if (g_gpu.EnumerateInstanceLayerProperties(&count, NULL) != VK_SUCCESS || count == 0) {
        return false;
    }
    layers = (VkLayerProperties *)calloc(count, sizeof(*layers));
    if (layers == NULL) {
        return false;
    }
    if (g_gpu.EnumerateInstanceLayerProperties(&count, layers) == VK_SUCCESS) {
        for (i = 0; i < count; ++i) {
            found = found || strcmp(layers[i].layerName, name) == 0;
        }
    }
    free(layers);
    return found;
}

static bool has_extension(const char *layer, const char *name) {
    uint32_t count = 0;
    VkExtensionProperties *extensions;
    bool found = false;
    uint32_t i;
    if (g_gpu.EnumerateInstanceExtensionProperties(layer, &count, NULL) != VK_SUCCESS || count == 0) {
        return false;
    }
    extensions = (VkExtensionProperties *)calloc(count, sizeof(*extensions));
    if (extensions == NULL) {
        return false;
    }
    if (g_gpu.EnumerateInstanceExtensionProperties(layer, &count, extensions) == VK_SUCCESS) {
        for (i = 0; i < count; ++i) {
            found = found || strcmp(extensions[i].extensionName, name) == 0;
        }
    }
    free(extensions);
    return found;
}

static bool pick_device(void) {
    uint32_t count = 0;
    VkPhysicalDevice *devices;
    uint32_t i;
    bool found = false;
    if (g_gpu.EnumeratePhysicalDevices(g_gpu.instance, &count, NULL) != VK_SUCCESS || count == 0) {
        return false;
    }
    devices = (VkPhysicalDevice *)calloc(count, sizeof(*devices));
    if (devices == NULL || g_gpu.EnumeratePhysicalDevices(g_gpu.instance, &count, devices) != VK_SUCCESS) {
        free(devices);
        return false;
    }
    for (i = 0; i < count && !found; ++i) {
        VkPhysicalDeviceProperties properties;
        VkQueueFamilyProperties families[16];
        uint32_t family_count = 16;
        uint32_t f;
        g_gpu.GetPhysicalDeviceProperties(devices[i], &properties);
        if (properties.apiVersion < VK_API_VERSION_1_1) {
            continue;
        }
        g_gpu.GetPhysicalDeviceQueueFamilyProperties(devices[i], &family_count, families);
        for (f = 0; f < family_count; ++f) {
            if ((families[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0u) {
                g_gpu.physical = devices[i];
                g_gpu.family = f;
                memcpy(g_gpu.device_name, properties.deviceName, sizeof(g_gpu.device_name));
                g_gpu.device_name[sizeof(g_gpu.device_name) - 1u] = '\0';
                found = true;
                break;
            }
        }
    }
    free(devices);
    return found;
}

static bool create_render_pass(void) {
    VkAttachmentDescription attachment;
    VkAttachmentReference reference;
    VkSubpassDescription subpass;
    VkSubpassDependency dependencies[2];
    VkRenderPassCreateInfo info;
    memset(&attachment, 0, sizeof(attachment));
    attachment.format = VK_FORMAT_R8G8B8A8_UNORM;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    attachment.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    reference.attachment = 0;
    reference.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&subpass, 0, sizeof(subpass));
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &reference;
    memset(dependencies, 0, sizeof(dependencies));
    /* The last frame's readback finishes before this frame's clear. */
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = 0;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    /* The readback waits for the colour writes. */
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &attachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 2;
    info.pDependencies = dependencies;
    return g_gpu.CreateRenderPass(g_gpu.device, &info, NULL, &g_gpu.render_pass) == VK_SUCCESS;
}

/* 1: ready. 0: SKIP, with the reason. -1: FAIL, with the reason. */
static int gpu_open(char *reason, size_t reason_size) {
    const char *layer = "VK_LAYER_KHRONOS_validation";
    const char *extension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    const char *skip_validation = getenv("NK_UI_VK_VALIDATION");
    VkApplicationInfo app;
    VkInstanceCreateInfo instance_info;
    VkDeviceQueueCreateInfo queue_info;
    VkDeviceCreateInfo device_info;
    VkCommandPoolCreateInfo pool_info;
    VkCommandBufferAllocateInfo cb_info;
    VkFenceCreateInfo fence_info;
    VkResult result;
    uint32_t version = 0;
    float priority = 1.0f;
    if (!loader_open()) {
#if defined(_WIN32)
        snprintf(reason, reason_size, "no Vulkan loader (vulkan-1.dll) could be loaded");
#else
        snprintf(reason, reason_size, "no Vulkan loader (libvulkan.so.1) could be loaded");
#endif
        return 0;
    }
#define LOAD_GLOBAL(name) \
    g_gpu.name = (PFN_vk##name)g_gpu.GetInstanceProcAddr(VK_NULL_HANDLE, "vk" #name);
    GLOBAL_FUNCS(LOAD_GLOBAL)
#undef LOAD_GLOBAL
    if (g_gpu.CreateInstance == NULL || g_gpu.EnumerateInstanceLayerProperties == NULL ||
        g_gpu.EnumerateInstanceExtensionProperties == NULL) {
        snprintf(reason, reason_size, "the Vulkan loader lacks its global entry points");
        return 0;
    }
    if (g_gpu.EnumerateInstanceVersion == NULL ||
        g_gpu.EnumerateInstanceVersion(&version) != VK_SUCCESS || version < VK_API_VERSION_1_1) {
        snprintf(reason, reason_size, "the Vulkan loader does not support Vulkan 1.1");
        return 0;
    }
    g_gpu.validation = (skip_validation == NULL || strcmp(skip_validation, "0") != 0) &&
                       has_layer(layer) && has_extension(NULL, extension);
    memset(&app, 0, sizeof(app));
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "test_nk_ui_vk";
    app.apiVersion = VK_API_VERSION_1_1;
    memset(&instance_info, 0, sizeof(instance_info));
    instance_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instance_info.pApplicationInfo = &app;
    if (g_gpu.validation) {
        instance_info.enabledLayerCount = 1;
        instance_info.ppEnabledLayerNames = &layer;
        instance_info.enabledExtensionCount = 1;
        instance_info.ppEnabledExtensionNames = &extension;
    }
    result = g_gpu.CreateInstance(&instance_info, NULL, &g_gpu.instance);
    if (result != VK_SUCCESS) {
        snprintf(reason, reason_size, "vkCreateInstance failed (VkResult %d): no usable Vulkan driver",
                 (int)result);
        g_gpu.instance = VK_NULL_HANDLE;
        return 0;
    }
#define LOAD_INSTANCE(name) \
    g_gpu.name = (PFN_vk##name)g_gpu.GetInstanceProcAddr(g_gpu.instance, "vk" #name); \
    if (g_gpu.name == NULL) { \
        snprintf(reason, reason_size, "vk" #name " is missing"); \
        return -1; \
    }
    INSTANCE_FUNCS(LOAD_INSTANCE)
#undef LOAD_INSTANCE
    if (g_gpu.validation) {
        VkDebugUtilsMessengerCreateInfoEXT messenger;
        g_gpu.CreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)
            g_gpu.GetInstanceProcAddr(g_gpu.instance, "vkCreateDebugUtilsMessengerEXT");
        g_gpu.DestroyDebugUtilsMessengerEXT = (PFN_vkDestroyDebugUtilsMessengerEXT)
            g_gpu.GetInstanceProcAddr(g_gpu.instance, "vkDestroyDebugUtilsMessengerEXT");
        memset(&messenger, 0, sizeof(messenger));
        messenger.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        messenger.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                    VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        messenger.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                                VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        messenger.pfnUserCallback = on_validation;
        if (g_gpu.CreateDebugUtilsMessengerEXT == NULL ||
            g_gpu.CreateDebugUtilsMessengerEXT(g_gpu.instance, &messenger, NULL,
                                               &g_gpu.messenger) != VK_SUCCESS) {
            snprintf(reason, reason_size, "the validation layer is present but its messenger failed");
            return -1;
        }
    }
    if (!pick_device()) {
        snprintf(reason, reason_size, "no Vulkan 1.1 device with a graphics queue");
        return 0;
    }
    g_gpu.GetPhysicalDeviceMemoryProperties(g_gpu.physical, &g_gpu.memory);
    memset(&queue_info, 0, sizeof(queue_info));
    queue_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_info.queueFamilyIndex = g_gpu.family;
    queue_info.queueCount = 1;
    queue_info.pQueuePriorities = &priority;
    memset(&device_info, 0, sizeof(device_info));
    device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    device_info.queueCreateInfoCount = 1;
    device_info.pQueueCreateInfos = &queue_info;
    result = g_gpu.CreateDevice(g_gpu.physical, &device_info, NULL, &g_gpu.device);
    if (result != VK_SUCCESS) {
        snprintf(reason, reason_size, "vkCreateDevice failed on %.128s (VkResult %d)",
                 g_gpu.device_name, (int)result);
        g_gpu.device = VK_NULL_HANDLE;
        return -1;
    }
#define LOAD_DEVICE(name) \
    g_gpu.name = (PFN_vk##name)g_gpu.GetDeviceProcAddr(g_gpu.device, "vk" #name); \
    if (g_gpu.name == NULL) { \
        snprintf(reason, reason_size, "vk" #name " is missing"); \
        return -1; \
    }
    DEVICE_FUNCS(LOAD_DEVICE)
#undef LOAD_DEVICE
    g_gpu.GetDeviceQueue(g_gpu.device, g_gpu.family, 0, &g_gpu.queue);
    memset(&pool_info, 0, sizeof(pool_info));
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = g_gpu.family;
    memset(&cb_info, 0, sizeof(cb_info));
    cb_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cb_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cb_info.commandBufferCount = 1;
    memset(&fence_info, 0, sizeof(fence_info));
    fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    if (g_gpu.CreateCommandPool(g_gpu.device, &pool_info, NULL, &g_gpu.pool) != VK_SUCCESS) {
        g_gpu.pool = VK_NULL_HANDLE;
        snprintf(reason, reason_size, "vkCreateCommandPool failed");
        return -1;
    }
    cb_info.commandPool = g_gpu.pool;
    if (g_gpu.AllocateCommandBuffers(g_gpu.device, &cb_info, &g_gpu.cb) != VK_SUCCESS ||
        g_gpu.CreateFence(g_gpu.device, &fence_info, NULL, &g_gpu.fence) != VK_SUCCESS ||
        !create_render_pass()) {
        snprintf(reason, reason_size, "creating the command buffer, fence or render pass failed");
        return -1;
    }
    return 1;
}

static void gpu_close(void) {
    if (g_gpu.device != VK_NULL_HANDLE) {
        g_gpu.DeviceWaitIdle(g_gpu.device);
        if (g_gpu.render_pass != VK_NULL_HANDLE) {
            g_gpu.DestroyRenderPass(g_gpu.device, g_gpu.render_pass, NULL);
        }
        if (g_gpu.fence != VK_NULL_HANDLE) {
            g_gpu.DestroyFence(g_gpu.device, g_gpu.fence, NULL);
        }
        if (g_gpu.pool != VK_NULL_HANDLE) {
            g_gpu.DestroyCommandPool(g_gpu.device, g_gpu.pool, NULL);
        }
        g_gpu.DestroyDevice(g_gpu.device, NULL);
    }
    if (g_gpu.messenger != VK_NULL_HANDLE && g_gpu.DestroyDebugUtilsMessengerEXT != NULL) {
        g_gpu.DestroyDebugUtilsMessengerEXT(g_gpu.instance, g_gpu.messenger, NULL);
    }
    if (g_gpu.instance != VK_NULL_HANDLE && g_gpu.DestroyInstance != NULL) {
        g_gpu.DestroyInstance(g_gpu.instance, NULL);
    }
    loader_close();
}

/* ---- offscreen target ---- */

typedef struct {
    int w;
    int h;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    VkFramebuffer framebuffer;
    VkBuffer readback;
    VkDeviceMemory readback_memory;
    const uint8_t *pixels;
    bool coherent;
} Target;

static void target_close(Target *t) {
    if (t->framebuffer != VK_NULL_HANDLE) g_gpu.DestroyFramebuffer(g_gpu.device, t->framebuffer, NULL);
    if (t->view != VK_NULL_HANDLE) g_gpu.DestroyImageView(g_gpu.device, t->view, NULL);
    if (t->image != VK_NULL_HANDLE) g_gpu.DestroyImage(g_gpu.device, t->image, NULL);
    if (t->memory != VK_NULL_HANDLE) g_gpu.FreeMemory(g_gpu.device, t->memory, NULL);
    if (t->pixels != NULL) g_gpu.UnmapMemory(g_gpu.device, t->readback_memory);
    if (t->readback != VK_NULL_HANDLE) g_gpu.DestroyBuffer(g_gpu.device, t->readback, NULL);
    if (t->readback_memory != VK_NULL_HANDLE) g_gpu.FreeMemory(g_gpu.device, t->readback_memory, NULL);
    memset(t, 0, sizeof(*t));
}

static bool target_open(Target *t, int w, int h) {
    VkImageCreateInfo image;
    VkMemoryRequirements requirements;
    VkMemoryAllocateInfo allocate;
    VkImageViewCreateInfo view;
    VkFramebufferCreateInfo framebuffer;
    VkBufferCreateInfo buffer;
    void *mapped = NULL;
    uint32_t type;
    memset(t, 0, sizeof(*t));
    t->w = w;
    t->h = h;
    memset(&image, 0, sizeof(image));
    image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image.imageType = VK_IMAGE_TYPE_2D;
    image.format = VK_FORMAT_R8G8B8A8_UNORM;
    image.extent.width = (uint32_t)w;
    image.extent.height = (uint32_t)h;
    image.extent.depth = 1;
    image.mipLevels = 1;
    image.arrayLayers = 1;
    image.samples = VK_SAMPLE_COUNT_1_BIT;
    image.tiling = VK_IMAGE_TILING_OPTIMAL;
    image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (g_gpu.CreateImage(g_gpu.device, &image, NULL, &t->image) != VK_SUCCESS) {
        t->image = VK_NULL_HANDLE;
        return false;
    }
    g_gpu.GetImageMemoryRequirements(g_gpu.device, t->image, &requirements);
    type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) type = memory_type(requirements.memoryTypeBits, 0);
    memset(&allocate, 0, sizeof(allocate));
    allocate.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (type == UINT32_MAX || g_gpu.AllocateMemory(g_gpu.device, &allocate, NULL, &t->memory) != VK_SUCCESS ||
        g_gpu.BindImageMemory(g_gpu.device, t->image, t->memory, 0) != VK_SUCCESS) {
        target_close(t);
        return false;
    }
    memset(&view, 0, sizeof(view));
    view.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view.image = t->image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view.subresourceRange.levelCount = 1;
    view.subresourceRange.layerCount = 1;
    if (g_gpu.CreateImageView(g_gpu.device, &view, NULL, &t->view) != VK_SUCCESS) {
        t->view = VK_NULL_HANDLE;
        target_close(t);
        return false;
    }
    memset(&framebuffer, 0, sizeof(framebuffer));
    framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer.renderPass = g_gpu.render_pass;
    framebuffer.attachmentCount = 1;
    framebuffer.pAttachments = &t->view;
    framebuffer.width = (uint32_t)w;
    framebuffer.height = (uint32_t)h;
    framebuffer.layers = 1;
    if (g_gpu.CreateFramebuffer(g_gpu.device, &framebuffer, NULL, &t->framebuffer) != VK_SUCCESS) {
        t->framebuffer = VK_NULL_HANDLE;
        target_close(t);
        return false;
    }
    memset(&buffer, 0, sizeof(buffer));
    buffer.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer.size = (VkDeviceSize)w * (VkDeviceSize)h * 4u;
    buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (g_gpu.CreateBuffer(g_gpu.device, &buffer, NULL, &t->readback) != VK_SUCCESS) {
        t->readback = VK_NULL_HANDLE;
        target_close(t);
        return false;
    }
    g_gpu.GetBufferMemoryRequirements(g_gpu.device, t->readback, &requirements);
    type = memory_type(requirements.memoryTypeBits,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    t->coherent = type != UINT32_MAX;
    if (type == UINT32_MAX) type = memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    allocate.allocationSize = requirements.size;
    allocate.memoryTypeIndex = type;
    if (type == UINT32_MAX ||
        g_gpu.AllocateMemory(g_gpu.device, &allocate, NULL, &t->readback_memory) != VK_SUCCESS ||
        g_gpu.BindBufferMemory(g_gpu.device, t->readback, t->readback_memory, 0) != VK_SUCCESS ||
        g_gpu.MapMemory(g_gpu.device, t->readback_memory, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS) {
        target_close(t);
        return false;
    }
    t->pixels = (const uint8_t *)mapped;
    return true;
}

/* 0xRRGGBBAA, as the SDL_Renderer selftest reads its surface. */
static uint32_t target_pixel(const Target *t, int x, int y) {
    const uint8_t *p = t->pixels + ((size_t)y * (size_t)t->w + (size_t)x) * 4u;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static int channel(uint32_t pixel, int shift) {
    return (int)((pixel >> shift) & 0xFFu);
}

static bool color_close(uint32_t a, uint32_t b, int tolerance) {
    int shift;
    for (shift = 0; shift < 32; shift += 8) {
        if (abs(channel(a, shift) - channel(b, shift)) > tolerance) {
            return false;
        }
    }
    return true;
}

static NkUiVk *backend_create(const NkUiTextBackend *text) {
    NkUiVkInit init;
    memset(&init, 0, sizeof(init));
    init.get_instance_proc_addr = g_gpu.GetInstanceProcAddr;
    init.instance = g_gpu.instance;
    init.physical_device = g_gpu.physical;
    init.device = g_gpu.device;
    init.queue_family = g_gpu.family;
    init.queue = g_gpu.queue;
    init.render_pass = g_gpu.render_pass;
    init.subpass = 0;
    init.samples = VK_SAMPLE_COUNT_1_BIT;
    init.frames_in_flight = FRAMES;
    init.text = text;
    return nk_ui_vk_create(&init);
}

/* One frame, as a host records it: prepare outside the render pass, render
   inside it, then the target is copied out and read back. `drawn` is what
   prepare is given and `rendered` what render is given (normally the same).
   Returns false only when the harness itself fails. */
static bool frame(Target *t, NkUiVk *vk, const NkUiDrawList *drawn, const NkUiDrawList *rendered,
                  uint32_t clear_rgba, bool *prepare_ok, bool *render_ok) {
    VkCommandBufferBeginInfo begin;
    VkRenderPassBeginInfo pass;
    VkClearValue clear;
    VkBufferImageCopy copy;
    VkBufferMemoryBarrier barrier;
    VkSubmitInfo submit;
    VkCommandBuffer cb = g_gpu.cb;
    *prepare_ok = false;
    *render_ok = false;
    if (g_gpu.ResetCommandBuffer(cb, 0) != VK_SUCCESS) {
        return false;
    }
    memset(&begin, 0, sizeof(begin));
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (g_gpu.BeginCommandBuffer(cb, &begin) != VK_SUCCESS) {
        return false;
    }
    *prepare_ok = nk_ui_vk_prepare(vk, drawn, cb, g_frame % FRAMES);
    memset(&clear, 0, sizeof(clear));
    clear.color.float32[0] = (float)channel(clear_rgba, 24) / 255.0f;
    clear.color.float32[1] = (float)channel(clear_rgba, 16) / 255.0f;
    clear.color.float32[2] = (float)channel(clear_rgba, 8) / 255.0f;
    clear.color.float32[3] = (float)channel(clear_rgba, 0) / 255.0f;
    memset(&pass, 0, sizeof(pass));
    pass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    pass.renderPass = g_gpu.render_pass;
    pass.framebuffer = t->framebuffer;
    pass.renderArea.extent.width = (uint32_t)t->w;
    pass.renderArea.extent.height = (uint32_t)t->h;
    pass.clearValueCount = 1;
    pass.pClearValues = &clear;
    g_gpu.CmdBeginRenderPass(cb, &pass, VK_SUBPASS_CONTENTS_INLINE);
    if (*prepare_ok) {
        *render_ok = nk_ui_vk_render(vk, rendered, cb, (uint32_t)t->w, (uint32_t)t->h);
    }
    g_gpu.CmdEndRenderPass(cb);
    memset(&copy, 0, sizeof(copy));
    copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copy.imageSubresource.layerCount = 1;
    copy.imageExtent.width = (uint32_t)t->w;
    copy.imageExtent.height = (uint32_t)t->h;
    copy.imageExtent.depth = 1;
    g_gpu.CmdCopyImageToBuffer(cb, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->readback, 1, &copy);
    memset(&barrier, 0, sizeof(barrier));
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = t->readback;
    barrier.size = VK_WHOLE_SIZE;
    g_gpu.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                             0, NULL, 1, &barrier, 0, NULL);
    if (g_gpu.EndCommandBuffer(cb) != VK_SUCCESS || g_gpu.ResetFences(g_gpu.device, 1, &g_gpu.fence) != VK_SUCCESS) {
        return false;
    }
    memset(&submit, 0, sizeof(submit));
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cb;
    if (g_gpu.QueueSubmit(g_gpu.queue, 1, &submit, g_gpu.fence) != VK_SUCCESS ||
        g_gpu.WaitForFences(g_gpu.device, 1, &g_gpu.fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
        return false;
    }
    if (!t->coherent) {
        VkMappedMemoryRange range;
        memset(&range, 0, sizeof(range));
        range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
        range.memory = t->readback_memory;
        range.size = VK_WHOLE_SIZE;
        if (g_gpu.InvalidateMappedMemoryRanges(g_gpu.device, 1, &range) != VK_SUCCESS) {
            return false;
        }
    }
    ++g_frame;
    return true;
}

/* A frame that must prepare and render. */
static bool draw(Target *t, NkUiVk *vk, const NkUiDrawList *list, uint32_t clear_rgba) {
    bool prepared = false;
    bool rendered = false;
    return frame(t, vk, list, list, clear_rgba, &prepared, &rendered) && prepared && rendered;
}

/* ---- a synthetic text engine ---- */

/* Font 0: one 8 x 10 glyph per byte, 10 px apart, a 16 px line with the
   baseline 12 px down; a space has no bitmap. Every texel's coverage differs, and
   the rows are stored 11 bytes apart with marker bytes in the gap, so a backend
   that ignored the pitch would show them. Font 1: 400 x 400 glyphs, all drawn
   at the run's origin, clear but for a 4 x 4 block whose place names the glyph,
   for filling the atlas. */
#define TEXT_ADVANCE 10
#define TEXT_LINE 16
#define TEXT_BASELINE 12
#define GLYPH_W 8
#define GLYPH_H 10
#define GLYPH_PITCH 11
#define BIG_SIDE 400
#define BIG_FIRST 0x30u
#define BIG_COUNT 40u

static uint8_t g_bitmap[BIG_SIDE * BIG_SIDE];
static int g_raster_calls = 0;

static uint8_t coverage(uint32_t glyph, int row, int col) {
    return (uint8_t)((glyph * 53u + (uint32_t)row * 29u + (uint32_t)col * 17u) & 0xFFu);
}

static void big_block(uint32_t glyph, int *x, int *y) {
    uint32_t k = glyph - BIG_FIRST;
    *x = (int)(k % 8u) * 40 + 8;
    *y = (int)(k / 8u) * 40 + 8;
}

static NkUiTextSize text_measure(void *user, int font, int px, const char *s, size_t n) {
    NkUiTextSize size;
    (void)user;
    (void)px;
    (void)s;
    size.width = font == 1 ? (float)BIG_SIDE : (float)(n * TEXT_ADVANCE);
    size.height = font == 1 ? (float)BIG_SIDE : (float)TEXT_LINE;
    size.baseline = font == 1 ? (float)BIG_SIDE : (float)TEXT_BASELINE;
    return size;
}

static int text_shape(void *user, int font, int px, const char *s, size_t n, NkUiGlyph *out, int max) {
    int i;
    (void)user;
    (void)px;
    for (i = 0; i < max && (size_t)i < n; ++i) {
        memset(&out[i], 0, sizeof(out[i]));
        out[i].glyph = (uint8_t)s[i];
        out[i].codepoint = (uint8_t)s[i];
        out[i].x = font == 1 ? 0.0f : (float)(i * TEXT_ADVANCE);
        out[i].advance_x = font == 1 ? 0.0f : (float)TEXT_ADVANCE;
    }
    return i;
}

static bool text_raster(void *user, int font, int px, uint32_t glyph, NkUiGlyphBitmap *out) {
    (void)user;
    (void)px;
    ++g_raster_calls;
    memset(out, 0, sizeof(*out));
    if (font == 1) {
        int bx;
        int by;
        int r;
        if (glyph < BIG_FIRST || glyph >= BIG_FIRST + BIG_COUNT) {
            return false;
        }
        memset(g_bitmap, 0, sizeof(g_bitmap));
        big_block(glyph, &bx, &by);
        for (r = 0; r < 4; ++r) {
            memset(g_bitmap + (size_t)(by + r) * BIG_SIDE + (size_t)bx, 0xFF, 4u);
        }
        out->pixels = g_bitmap;
        out->width = BIG_SIDE;
        out->height = BIG_SIDE;
        out->pitch = BIG_SIDE;
        out->bearing_y = BIG_SIDE;
        return true;
    }
    if (glyph == ' ') {
        return false;
    }
    {
        int r;
        int c;
        memset(g_bitmap, 0xEE, (size_t)GLYPH_PITCH * GLYPH_H);
        for (r = 0; r < GLYPH_H; ++r) {
            for (c = 0; c < GLYPH_W; ++c) {
                g_bitmap[r * GLYPH_PITCH + c] = coverage(glyph, r, c);
            }
        }
    }
    out->pixels = g_bitmap;
    out->width = GLYPH_W;
    out->height = GLYPH_H;
    out->pitch = GLYPH_PITCH;
    out->bearing_x = 0;
    out->bearing_y = GLYPH_H;
    out->advance_x = (float)TEXT_ADVANCE;
    return true;
}

static const NkUiTextBackend g_text = {NULL, text_measure, text_shape, text_raster};

/* A font 0 texel drawn in `rgba` over opaque black. Where it lands: a run's
   16 px line box fills its 16 px bounds, so the baseline is 12 px below the
   bounds' top and a glyph's top row 2 px below it, glyph i 10 px further right
   than glyph i - 1. */
static uint32_t text_over_black(uint32_t glyph, int row, int col, uint32_t rgba) {
    uint32_t a = coverage(glyph, row, col);
    uint32_t r = ((rgba >> 24) & 0xFFu) * a / 255u;
    uint32_t g = ((rgba >> 16) & 0xFFu) * a / 255u;
    uint32_t b = ((rgba >> 8) & 0xFFu) * a / 255u;
    return (r << 24) | (g << 16) | (b << 8) | 0xFFu;
}

/* ---- tests ---- */

static int test_availability(void) {
    NkUiVkInit init;
    NkUiVk *vk;
    CHECK(nk_ui_vk_available(), "this build includes the Vulkan backend");
    CHECK(nk_ui_vk_create(NULL) == NULL, "no init, no backend");
    memset(&init, 0, sizeof(init));
    CHECK(nk_ui_vk_create(&init) == NULL, "an empty init is refused");
    vk = backend_create(NULL);
    CHECK(vk != NULL, "create a backend on the test device");
    nk_ui_vk_destroy(vk);
    init.get_instance_proc_addr = g_gpu.GetInstanceProcAddr;
    init.instance = g_gpu.instance;
    init.physical_device = g_gpu.physical;
    init.device = g_gpu.device;
    init.queue_family = g_gpu.family;
    init.queue = g_gpu.queue;
    init.render_pass = g_gpu.render_pass;
    init.frames_in_flight = 0;
    CHECK(nk_ui_vk_create(&init) == NULL, "zero frames in flight is refused");
    init.frames_in_flight = NK_UI_VK_MAX_FRAMES + 1u;
    CHECK(nk_ui_vk_create(&init) == NULL, "too many frames in flight is refused");
    init.frames_in_flight = 1;
    init.queue_family = 0xFFFFu;
    CHECK(nk_ui_vk_create(&init) == NULL, "a queue family the device lacks is refused");
    nk_ui_vk_destroy(NULL);
    return 1;
}

/* Scissors: a rect half outside one is cut to it, nested scissors intersect,
   a command whose clip is empty draws nothing, SCISSOR_END restores the scissor
   around it, and a clip past the framebuffer's edge is clamped to it. */
static int test_scissor(void) {
    static uint8_t white_pixels[4] = {0xFF, 0xFF, 0xFF, 0xFF};
    NkUiImage white = {1u, 1u, white_pixels, 0x5C155012ull};
    const uint32_t black = 0x000000FFu;
    const uint32_t red = 0xFF0000FFu;
    const uint32_t blue = 0x0000FFFFu;
    const uint32_t green = 0x00FF00FFu;
    const uint32_t yellow = 0xFFFF00FFu;
    const uint32_t magenta = 0xFF00FFFFu;
    Target target;
    NkUiDrawBuffer buffer;
    NkUiVkStats stats;
    NkUiVk *vk;
    memset(&buffer, 0, sizeof(buffer));
    CHECK(target_open(&target, 96, 96), "open a 96 x 96 target");
    vk = backend_create(&g_text);
    CHECK(vk != NULL, "create the backend");
    /* A rect half outside a scissor. */
    CHECK(nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){10, 10, 30, 30}, 1u), "scissor on");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){0.0f, 0.0f, 64.0f, 64.0f}, 1.0f, 0.0f, red, 2u),
          "a red rect larger than the scissor");
    CHECK(nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){10, 10, 30, 30}, 1u), "scissor off");
    /* After the end, nothing clips. */
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){40.0f, 40.0f, 10.0f, 10.0f}, 1.0f, 0.0f, blue, 3u),
          "an unclipped blue rect");
    /* Nested scissors intersect; closing the inner one restores the outer. */
    CHECK(nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){50, 0, 90, 30}, 4u) &&
              nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){70, 10, 96, 40}, 5u),
          "two nested scissors");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){40.0f, 0.0f, 56.0f, 48.0f}, 1.0f, 0.0f, green, 6u),
          "a green rect across both");
    CHECK(nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){70, 10, 96, 40}, 5u), "inner off");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){52.0f, 2.0f, 8.0f, 8.0f}, 1.0f, 0.0f, yellow, 7u),
          "a yellow rect inside the outer scissor only");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){75.0f, 32.0f, 4.0f, 4.0f}, 1.0f, 0.0f,
                              0xFFFFFFFFu, 8u), "a white rect below the outer scissor");
    CHECK(nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){50, 0, 90, 30}, 4u), "outer off");
    /* Disjoint nested scissors hide everything inside them. */
    CHECK(nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){0, 60, 20, 80}, 9u) &&
              nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){30, 60, 50, 80}, 10u),
          "two disjoint scissors");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){0.0f, 60.0f, 96.0f, 36.0f}, 1.0f, 0.0f,
                              0xFFFFFFFFu, 11u) &&
              nk_ui_draw_add_text(&buffer, (NkUiPixelRect){0, 60, 40, 76}, "AB", 2, 0, 16,
                                  0xFFFFFFFFu, 12u) &&
              nk_ui_draw_add_image(&buffer, &white, (NkUiPixelRect){0, 60, 96, 96}, 0xFFFFFFFFu, 13u),
          "a rect, text and an image under an empty clip");
    CHECK(nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){30, 60, 50, 80}, 10u) &&
              nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){0, 60, 20, 80}, 9u),
          "both off");
    /* A clip that starts left of the framebuffer and ends below it. */
    CHECK(nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){-50, 85, 200, 200}, 14u), "wide scissor");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){-20.0f, 85.0f, 60.0f, 20.0f}, 1.0f, 0.0f, magenta, 15u),
          "a magenta rect hanging off the left and bottom edges");
    CHECK(nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){-50, 85, 200, 200}, 14u), "wide off");

    CHECK(draw(&target, vk, &buffer.list, black), "prepare and render the scissor list");
    CHECK(target_pixel(&target, 20, 20) == red && target_pixel(&target, 10, 10) == red &&
          target_pixel(&target, 29, 29) == red, "inside the scissor is drawn");
    CHECK(target_pixel(&target, 5, 5) == black && target_pixel(&target, 9, 20) == black &&
          target_pixel(&target, 30, 20) == black && target_pixel(&target, 20, 30) == black,
          "outside the scissor is clipped; its far edges are exclusive");
    CHECK(target_pixel(&target, 45, 45) == blue && target_pixel(&target, 39, 45) == black &&
          target_pixel(&target, 50, 45) == black, "after SCISSOR_END the rect draws whole");
    CHECK(target_pixel(&target, 80, 20) == green && target_pixel(&target, 70, 10) == green &&
          target_pixel(&target, 89, 29) == green, "nested scissors draw their intersection");
    CHECK(target_pixel(&target, 60, 20) == black && target_pixel(&target, 69, 20) == black &&
          target_pixel(&target, 90, 20) == black && target_pixel(&target, 80, 9) == black &&
          target_pixel(&target, 80, 30) == black && target_pixel(&target, 80, 35) == black,
          "outside either nested scissor stays clear");
    CHECK(target_pixel(&target, 55, 5) == yellow && target_pixel(&target, 52, 2) == yellow,
          "closing the inner scissor restores the outer one");
    CHECK(target_pixel(&target, 76, 33) == black, "the outer scissor still clips");
    CHECK(target_pixel(&target, 10, 70) == black && target_pixel(&target, 40, 70) == black &&
          target_pixel(&target, 90, 90) == black, "an empty clip draws nothing");
    CHECK(target_pixel(&target, 0, 90) == magenta && target_pixel(&target, 39, 95) == magenta &&
          target_pixel(&target, 40, 90) == black && target_pixel(&target, 10, 84) == black,
          "a clip past the framebuffer is clamped to it");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.commands_clipped == 4u, "the four hidden commands were skipped, not drawn");
    CHECK(stats.text_runs_drawn == 0u && stats.images_drawn == 0u && stats.atlas_uploads == 0u,
          "the hidden text and image did no work");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_vk_destroy(vk);
    target_close(&target);
    return 1;
}

/* Premultiplied blending: ONE, ONE_MINUS_SRC_ALPHA on colour and alpha. A
   half-transparent red rect over transparent black stays (128, 0, 0, 128),
   where straight-alpha blending would halve it again; a premultiplied colour
   with zero alpha adds light; over white the red mixes as expected. */
static int test_premultiplied_blend(void) {
    static const NkUiVertex vertices[8] = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0x80000080u}, {16.0f, 0.0f, 0.0f, 0.0f, 0x80000080u},
        {16.0f, 16.0f, 0.0f, 0.0f, 0x80000080u}, {0.0f, 16.0f, 0.0f, 0.0f, 0x80000080u},
        {16.0f, 0.0f, 0.0f, 0.0f, 0x40404000u}, {32.0f, 0.0f, 0.0f, 0.0f, 0x40404000u},
        {32.0f, 16.0f, 0.0f, 0.0f, 0x40404000u}, {16.0f, 16.0f, 0.0f, 0.0f, 0x40404000u},
    };
    static const uint32_t indices[12] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    NkUiDrawCmd commands[2];
    NkUiDrawList list;
    Target target;
    NkUiVk *vk;
    memset(commands, 0, sizeof(commands));
    commands[0].kind = NK_UI_DRAW_RECTANGLE;
    commands[0].first = 0;
    commands[0].count = 6;
    commands[0].clip[2] = 16;
    commands[0].clip[3] = 16;
    commands[1].kind = NK_UI_DRAW_RECTANGLE;
    commands[1].first = 6;
    commands[1].count = 6;
    commands[1].clip[0] = 16;
    commands[1].clip[2] = 32;
    commands[1].clip[3] = 16;
    memset(&list, 0, sizeof(list));
    list.v = vertices;
    list.nv = 8;
    list.idx = indices;
    list.ni = 12;
    list.cmd = commands;
    list.ncmd = 2;
    CHECK(target_open(&target, 32, 16), "open a 32 x 16 target");
    vk = backend_create(NULL);
    CHECK(vk != NULL, "create the backend");
    CHECK(draw(&target, vk, &list, 0x00000000u), "draw over transparent black");
    CHECK(color_close(target_pixel(&target, 8, 8), 0x80000080u, 1),
          "50% red over transparent black is (128, 0, 0, 128): ONE, not SRC_ALPHA");
    CHECK(color_close(target_pixel(&target, 24, 8), 0x40404000u, 1),
          "a premultiplied colour with zero alpha adds light");
    CHECK(draw(&target, vk, &list, 0xFFFFFFFFu), "draw over opaque white");
    CHECK(color_close(target_pixel(&target, 8, 8), 0xFF7F7FFFu, 1), "50% red over white");
    CHECK(target_pixel(&target, 24, 8) == 0xFFFFFFFFu, "added light saturates at white");
    nk_ui_vk_destroy(vk);
    target_close(&target);
    return 1;
}

/* The rounded fill's fringe fades to the background without a colour halo. */
static int test_rounded_corner_fringe(void) {
    Target target;
    NkUiDrawBuffer buffer;
    NkUiVk *vk;
    uint32_t edge;
    memset(&buffer, 0, sizeof(buffer));
    CHECK(target_open(&target, 64, 64), "open a 64 x 64 target");
    vk = backend_create(NULL);
    CHECK(vk != NULL, "create the backend");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){8.0f, 8.0f, 48.0f, 48.0f}, 1.0f, 12.0f,
                              0xFFFFFFFFu, 1u), "rounded white rect");
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render the rounded rect");
    CHECK(target_pixel(&target, 32, 32) == 0xFFFFFFFFu, "the middle is solid");
    CHECK(target_pixel(&target, 8, 8) == 0x000000FFu, "the cut corner stays background");
    CHECK(target_pixel(&target, 8, 32) == 0xFFFFFFFFu && target_pixel(&target, 55, 32) == 0xFFFFFFFFu &&
          target_pixel(&target, 7, 32) == 0x000000FFu && target_pixel(&target, 56, 32) == 0x000000FFu,
          "straight edges cover whole pixels");
    /* (11.5, 11.5) is 12.02 px from the corner's centre (20, 20): inside the
       fringe band from 11.5 to 12.5 px. */
    edge = target_pixel(&target, 11, 11);
    CHECK(channel(edge, 24) > 0 && channel(edge, 24) < 255, "the curve is antialiased");
    CHECK(channel(edge, 16) == channel(edge, 24) && channel(edge, 8) == channel(edge, 24),
          "fringe pixels stay neutral grey: premultiplied blending, no colour halo");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_vk_destroy(vk);
    target_close(&target);
    return 1;
}

/* Text through the glyph atlas: every texel of every glyph lands where the
   SDL_Renderer backend puts it, coloured by the run and blended over the
   target; a blank glyph draws nothing; a scissor cuts a glyph; glyphs upload
   once and are reused. */
static int test_text(void) {
    Target target;
    NkUiDrawBuffer buffer;
    NkUiVkStats stats;
    NkUiVk *vk;
    int r;
    int c;
    memset(&buffer, 0, sizeof(buffer));
    g_raster_calls = 0;
    CHECK(target_open(&target, 96, 48), "open a 96 x 48 target");
    vk = backend_create(&g_text);
    CHECK(vk != NULL, "create the backend with the synthetic text engine");
    CHECK(nk_ui_draw_add_text(&buffer, (NkUiPixelRect){4, 4, 40, 20}, "AB", 2, 0, 16,
                              0xFFFFFFFFu, 1u), "white AB");
    CHECK(nk_ui_draw_add_text(&buffer, (NkUiPixelRect){4, 24, 40, 40}, " C", 2, 0, 16,
                              0xFF8000FFu, 2u), "orange space and C");
    CHECK(nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){60, 0, 70, 48}, 3u) &&
              nk_ui_draw_add_text(&buffer, (NkUiPixelRect){64, 4, 96, 20}, "D", 1, 0, 16,
                                  0xFFFFFFFFu, 4u) &&
              nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){60, 0, 70, 48}, 3u),
          "a D across a scissor's edge");
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render the text");
    for (r = 0; r < GLYPH_H; ++r) {
        for (c = 0; c < GLYPH_W; ++c) {
            CHECK(color_close(target_pixel(&target, 4 + c, 6 + r), text_over_black('A', r, c, 0xFFFFFFFFu), 1),
                  "every texel of A");
            CHECK(color_close(target_pixel(&target, 14 + c, 6 + r), text_over_black('B', r, c, 0xFFFFFFFFu), 1),
                  "every texel of B, one advance on");
            CHECK(color_close(target_pixel(&target, 14 + c, 26 + r), text_over_black('C', r, c, 0xFF8000FFu), 1),
                  "every texel of C, in the run's colour");
            if (c < 6) {
                CHECK(color_close(target_pixel(&target, 64 + c, 6 + r), text_over_black('D', r, c, 0xFFFFFFFFu), 1),
                      "D inside the scissor");
            } else {
                CHECK(target_pixel(&target, 64 + c, 6 + r) == 0x000000FFu, "D cut at the scissor");
            }
        }
    }
    CHECK(target_pixel(&target, 12, 10) == 0x000000FFu && target_pixel(&target, 13, 10) == 0x000000FFu,
          "nothing between the glyphs: the bitmap pitch is honoured");
    CHECK(target_pixel(&target, 4, 30) == 0x000000FFu && target_pixel(&target, 11, 30) == 0x000000FFu,
          "the space draws nothing");
    CHECK(target_pixel(&target, 4, 5) == 0x000000FFu && target_pixel(&target, 4, 16) == 0x000000FFu,
          "the glyph sits on the baseline");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.atlas_uploads == 4u && stats.atlas_pages == 1u && stats.atlas_glyphs == 5u,
          "four glyphs uploaded to one page; the blank space is cached too");
    CHECK(stats.glyphs_drawn == 4u && stats.text_runs_drawn == 3u && g_raster_calls == 5,
          "three runs, four glyph quads, five rasterisations");
    /* Over blue, the coverage blends: (a, a, 255). */
    CHECK(draw(&target, vk, &buffer.list, 0x0000FFFFu), "render the text over blue");
    for (c = 0; c < GLYPH_W; ++c) {
        uint32_t a = coverage('A', 3, c);
        CHECK(color_close(target_pixel(&target, 4 + c, 9), (a << 24) | (a << 16) | 0xFFFFu, 1),
              "white text over blue blends premultiplied");
    }
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.atlas_uploads == 4u && g_raster_calls == 5 && stats.glyphs_drawn == 8u,
          "the second frame reuses every glyph");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_vk_destroy(vk);
    target_close(&target);
    return 1;
}

/* A run whose glyphs overflow the atlas's eight pages: the atlas is dropped
   and started again in the middle of the run, and every glyph still draws,
   including those on pages dropped during the frame. */
static int test_atlas_page_overflow(void) {
    char run[BIG_COUNT];
    Target target;
    NkUiDrawBuffer buffer;
    NkUiVkStats stats;
    NkUiVk *vk;
    uint32_t k;
    memset(&buffer, 0, sizeof(buffer));
    for (k = 0; k < BIG_COUNT; ++k) {
        run[k] = (char)(BIG_FIRST + k);
    }
    CHECK(target_open(&target, 448, 448), "open a 448 x 448 target");
    vk = backend_create(&g_text);
    CHECK(vk != NULL, "create the backend");
    CHECK(nk_ui_draw_add_text(&buffer, (NkUiPixelRect){0, 0, 448, 448}, run, BIG_COUNT, 1, 400,
                              0xFFFFFFFFu, 1u), "forty quarter-page glyphs");
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render past the page limit");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.atlas_rebuilds == 1u, "the atlas was dropped once when its pages ran out");
    CHECK(stats.atlas_pages == 2u && stats.glyphs_drawn == BIG_COUNT && stats.atlas_uploads == BIG_COUNT,
          "every glyph was uploaded and drawn");
    for (k = 0; k < BIG_COUNT; ++k) {
        int x;
        int y;
        big_block(BIG_FIRST + k, &x, &y);
        /* Bounds 448 high, line 400: the glyph's top is 24 px down. */
        CHECK(target_pixel(&target, x + 1, 24 + y + 1) == 0xFFFFFFFFu &&
              target_pixel(&target, x + 3, 24 + y + 3) == 0xFFFFFFFFu,
              "each glyph's block is drawn, from old pages and new");
        CHECK(target_pixel(&target, x + 5, 24 + y + 1) == 0x000000FFu, "and nothing beside it");
    }
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render the run again");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.atlas_rebuilds == 2u && stats.glyphs_drawn == 2u * BIG_COUNT,
          "a frame that needs more than the atlas holds rebuilds every time and still draws");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_vk_destroy(vk);
    target_close(&target);
    return 1;
}

static int test_atlas_rebuild_on_scale_change(void) {
    Target target;
    NkUiDrawBuffer buffer;
    NkUiVkStats stats;
    NkUiVk *vk;
    memset(&buffer, 0, sizeof(buffer));
    CHECK(target_open(&target, 48, 24), "open a 48 x 24 target");
    vk = backend_create(&g_text);
    CHECK(vk != NULL, "create the backend");
    nk_ui_vk_set_scale(vk, 1.0f);
    CHECK(nk_ui_draw_add_text(&buffer, (NkUiPixelRect){4, 4, 40, 20}, "AB", 2, 0, 16,
                              0xFFFFFFFFu, 1u), "white AB");
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render at 1x");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.atlas_pages == 1u && stats.atlas_glyphs == 2u && stats.atlas_rebuilds == 0u,
          "1x populates one page without a rebuild");
    nk_ui_vk_set_scale(vk, 1.0f);
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.atlas_pages == 1u && stats.atlas_rebuilds == 0u, "an unchanged scale keeps the atlas");
    nk_ui_vk_set_scale(vk, 2.0f);
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.atlas_pages == 0u && stats.atlas_glyphs == 0u && stats.atlas_rebuilds == 1u,
          "a scale change drops every page");
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render after the change");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.atlas_pages == 1u && stats.atlas_glyphs == 2u && stats.atlas_uploads == 4u,
          "the atlas is rebuilt lazily");
    CHECK(color_close(target_pixel(&target, 7, 9), text_over_black('A', 3, 3, 0xFFFFFFFFu), 1) &&
          color_close(target_pixel(&target, 17, 9), text_over_black('B', 3, 3, 0xFFFFFFFFu), 1),
          "the rebuilt atlas draws the same glyphs");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_vk_destroy(vk);
    target_close(&target);
    return 1;
}

/* An image command: the texture is uploaded once and reused, the tint
   multiplies the texels, a scissor crops it, a stale serial uploads again, a
   shrunk image gets an area-averaged copy per drawn size, and texels land
   exactly at 1:1. */
static int test_image_commands(void) {
    static uint8_t pixels[4u * 4u * 4u];
    static uint8_t texels[2u * 2u * 4u] = {
        0xFF, 0x00, 0x00, 0xFF,  0x00, 0xFF, 0x00, 0xFF,
        0x00, 0x00, 0xFF, 0xFF,  0x80, 0x00, 0x00, 0x80,
    };
    static uint8_t big[64u * 64u * 4u];
    const uint32_t black = 0x000000FFu;
    NkUiImage image;
    NkUiImage quad = {2u, 2u, texels, 0x5EED0010ull};
    NkUiImage glyph = {64u, 64u, big, 0x5EED0003ull};
    Target target;
    NkUiDrawBuffer buffer;
    NkUiVkStats stats;
    NkUiVk *vk;
    memset(&buffer, 0, sizeof(buffer));
    memset(pixels, 0xFF, sizeof(pixels));
    memset(big, 0xFF, sizeof(big));
    image.width = 4u;
    image.height = 4u;
    image.rgba_pm = pixels;
    image.serial = 0x5EED0001ull;
    CHECK(target_open(&target, 64, 64), "open a 64 x 64 target");
    vk = backend_create(NULL);
    CHECK(vk != NULL, "create the backend");
    CHECK(nk_ui_draw_add_image(&buffer, &image, (NkUiPixelRect){0, 0, 16, 16}, 0xFFFFFFFFu, 1u),
          "untinted image");
    CHECK(nk_ui_draw_add_image(&buffer, &image, (NkUiPixelRect){20, 0, 36, 16}, 0x00FF00FFu, 2u),
          "green-tinted image");
    CHECK(nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){40, 0, 48, 8}, 3u), "scissor on");
    CHECK(nk_ui_draw_add_image(&buffer, &image, (NkUiPixelRect){40, 0, 56, 16}, 0xFFFFFFFFu, 4u),
          "cropped image");
    CHECK(nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){40, 0, 48, 8}, 3u), "scissor off");
    CHECK(nk_ui_draw_add_image(&buffer, &quad, (NkUiPixelRect){0, 40, 2, 42}, 0xFFFFFFFFu, 5u),
          "a 2 x 2 image at 1:1");
    CHECK(draw(&target, vk, &buffer.list, black), "render the image list");
    CHECK(target_pixel(&target, 8, 8) == 0xFFFFFFFFu, "the image is drawn as it is");
    CHECK(target_pixel(&target, 28, 8) == 0x00FF00FFu, "the tint multiplies the texels");
    CHECK(target_pixel(&target, 44, 4) == 0xFFFFFFFFu && target_pixel(&target, 44, 12) == black &&
          target_pixel(&target, 52, 4) == black, "the scissor crops the image");
    CHECK(target_pixel(&target, 0, 40) == 0xFF0000FFu && target_pixel(&target, 1, 40) == 0x00FF00FFu &&
          target_pixel(&target, 0, 41) == 0x0000FFFFu, "texels land exactly at 1:1");
    CHECK(color_close(target_pixel(&target, 1, 41), 0x800000FFu, 1),
          "a premultiplied half-red texel over black");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.images_drawn == 4u && stats.image_uploads == 2u && stats.images_failed == 0u,
          "one upload serves every draw of the same image");
    CHECK(draw(&target, vk, &buffer.list, 0xFFFFFFFFu), "render again over white");
    CHECK(color_close(target_pixel(&target, 1, 41), 0xFF7F7FFFu, 1),
          "the half-red texel over white");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.images_drawn == 8u && stats.image_uploads == 2u, "the textures are cached across frames");
    image.serial = 0x5EED0002ull;
    CHECK(draw(&target, vk, &buffer.list, black), "render with a new serial");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.image_uploads == 3u, "a new serial is a new texture");
    nk_ui_draw_buffer_destroy(&buffer);
    /* A 64 x 64 image drawn at 24 px is uploaded once as an area-averaged 24 px
       copy; at 48 px it gets its own copy. */
    memset(&buffer, 0, sizeof(buffer));
    CHECK(nk_ui_draw_add_image(&buffer, &glyph, (NkUiPixelRect){0, 20, 24, 44}, 0xFFFFFFFFu, 6u) &&
              nk_ui_draw_add_image(&buffer, &glyph, (NkUiPixelRect){30, 20, 54, 44}, 0xFFFFFFFFu, 7u),
          "two shrunk draws");
    CHECK(draw(&target, vk, &buffer.list, black), "render the shrunk images");
    CHECK(target_pixel(&target, 12, 32) == 0xFFFFFFFFu && target_pixel(&target, 42, 32) == 0xFFFFFFFFu,
          "the shrunk copy is drawn");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.image_uploads == 4u, "one shrunk copy serves both 24 px draws");
    nk_ui_draw_buffer_destroy(&buffer);
    memset(&buffer, 0, sizeof(buffer));
    CHECK(nk_ui_draw_add_image(&buffer, &glyph, (NkUiPixelRect){0, 0, 48, 48}, 0xFFFFFFFFu, 8u),
          "a 48 px draw");
    CHECK(draw(&target, vk, &buffer.list, black), "render at 48 px");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.image_uploads == 5u, "another size is another copy");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_vk_destroy(vk);
    target_close(&target);
    return 1;
}

/* More images in one frame than the cache holds: the extra ones get textures
   for that frame only, every one still draws, and a later frame evicts the
   least recently drawn entries. */
static int test_image_cache_overflow(void) {
    enum { COUNT = 70 };
    static uint8_t colors[COUNT][4];
    static NkUiImage images[COUNT];
    Target target;
    NkUiDrawBuffer buffer;
    NkUiVkStats stats;
    NkUiVk *vk;
    int i;
    int pass;
    memset(&buffer, 0, sizeof(buffer));
    CHECK(target_open(&target, 32, 16), "open a 32 x 16 target");
    vk = backend_create(NULL);
    CHECK(vk != NULL, "create the backend");
    for (i = 0; i < COUNT; ++i) {
        colors[i][0] = (uint8_t)(i * 3);
        colors[i][1] = (uint8_t)(255 - i * 3);
        colors[i][2] = 0x80u;
        colors[i][3] = 0xFFu;
        images[i].width = 1u;
        images[i].height = 1u;
        images[i].rgba_pm = colors[i];
        images[i].serial = 0x1A6E0000ull + (uint64_t)i;
        CHECK(nk_ui_draw_add_image(&buffer, &images[i],
                                   (NkUiPixelRect){(i % 16) * 2, (i / 16) * 2, (i % 16) * 2 + 1,
                                                   (i / 16) * 2 + 1}, 0xFFFFFFFFu, (uint32_t)i + 1u),
              "queue a one-pixel image");
    }
    for (pass = 0; pass < 2; ++pass) {
        CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render seventy images");
        for (i = 0; i < COUNT; ++i) {
            uint32_t want = ((uint32_t)colors[i][0] << 24) | ((uint32_t)colors[i][1] << 16) | 0x80FFu;
            CHECK(target_pixel(&target, (i % 16) * 2, (i / 16) * 2) == want, "every image draws");
        }
    }
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.images_drawn == 2u * COUNT && stats.image_uploads == COUNT + 6u,
          "64 cached, six for their frame only, twice");
    nk_ui_draw_buffer_destroy(&buffer);
    memset(&buffer, 0, sizeof(buffer));
    for (i = 64; i < COUNT; ++i) {
        CHECK(nk_ui_draw_add_image(&buffer, &images[i], (NkUiPixelRect){0, 0, 1, 1}, 0xFFFFFFFFu, 1u),
              "queue one of the six");
    }
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render the six alone");
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "and again");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.image_uploads == COUNT + 12u, "the six now evict older entries and stay cached");
    CHECK(target_pixel(&target, 0, 0) == (((uint32_t)colors[69][0] << 24) |
                                          ((uint32_t)colors[69][1] << 16) | 0x80FFu),
          "the last one drawn is on top");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_vk_destroy(vk);
    target_close(&target);
    return 1;
}

static uint8_t sample_bilinear(const uint8_t *pixels, int w, int h, float fx, float fy, int c) {
    int x0 = (int)floorf(fx);
    int y0 = (int)floorf(fy);
    float tx = fx - (float)x0;
    float ty = fy - (float)y0;
    int x1 = x0 + 1;
    int y1 = y0 + 1;
    float top;
    float bottom;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > w - 1) x1 = w - 1;
    if (y1 > h - 1) y1 = h - 1;
    if (x0 > w - 1) x0 = w - 1;
    if (y0 > h - 1) y0 = h - 1;
    top = (float)pixels[((size_t)y0 * (size_t)w + (size_t)x0) * 4u + (size_t)c] * (1.0f - tx) +
          (float)pixels[((size_t)y0 * (size_t)w + (size_t)x1) * 4u + (size_t)c] * tx;
    bottom = (float)pixels[((size_t)y1 * (size_t)w + (size_t)x0) * 4u + (size_t)c] * (1.0f - tx) +
             (float)pixels[((size_t)y1 * (size_t)w + (size_t)x1) * 4u + (size_t)c] * tx;
    return (uint8_t)floorf(top * (1.0f - ty) + bottom * ty + 0.5f);
}

/* The waves: drawn from the CPU reference at a third of their size and scaled
   up with a linear filter, so every pixel matches the reference sampled
   bilinearly; an unchanged command is not rendered again; more waves commands
   than the backend keeps textures for fail the frame. */
static int test_waves(void) {
    static uint8_t reference[32u * 16u * 4u];
    NkUiWavesCommand waves;
    NkUiWavesCommand later;
    NkUiWavePalette palette;
    Target target;
    NkUiDrawBuffer buffer;
    NkUiVkStats stats;
    NkUiVk *vk;
    int brightest = 0;
    int x;
    int y;
    int i;
    bool prepared = true;
    bool rendered = true;
    memset(&buffer, 0, sizeof(buffer));
    CHECK(nk_ui_waves_palette(0u, &palette), "base wave palette");
    nk_ui_waves_command(37.0, 96.0f, 48.0f, &palette, &waves);
    nk_ui_waves_command(38.0, 96.0f, 48.0f, &palette, &later);
    CHECK(nk_ui_waves_render_command(&waves, 32u, 16u, reference, 32u * 4u), "CPU reference frame");
    CHECK(target_open(&target, 96, 64), "open a 96 x 64 target");
    vk = backend_create(NULL);
    CHECK(vk != NULL, "create the backend");
    CHECK(nk_ui_draw_add_custom(&buffer, NK_UI_CUSTOM_WAVES, &waves, (NkUiPixelRect){0, 8, 96, 56}, 1u),
          "a 96 x 48 waves command");
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render the waves");
    for (y = 0; y < 64; ++y) {
        for (x = 0; x < 96; ++x) {
            uint32_t pixel = target_pixel(&target, x, y);
            if (y < 8 || y >= 56) {
                CHECK(pixel == 0x000000FFu, "nothing outside the waves' bounds");
                continue;
            }
            {
                float fx = ((float)x + 0.5f) / 96.0f * 32.0f - 0.5f;
                float fy = ((float)(y - 8) + 0.5f) / 48.0f * 16.0f - 0.5f;
                uint32_t want = ((uint32_t)sample_bilinear(reference, 32, 16, fx, fy, 0) << 24) |
                                ((uint32_t)sample_bilinear(reference, 32, 16, fx, fy, 1) << 16) |
                                ((uint32_t)sample_bilinear(reference, 32, 16, fx, fy, 2) << 8) | 0xFFu;
                CHECK(color_close(pixel, want, 3), "the waves match the CPU reference, scaled up");
                if (channel(pixel, 16) > brightest) brightest = channel(pixel, 16);
            }
        }
    }
    CHECK(brightest > 0 && brightest < 0x60, "the waves add dim light");
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render the same waves again");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.waves_drawn == 2u && stats.waves_rendered == 1u,
          "an unchanged waves command reuses its texture");
    nk_ui_draw_buffer_reset(&buffer);
    CHECK(nk_ui_draw_add_custom(&buffer, NK_UI_CUSTOM_WAVES, &later, (NkUiPixelRect){0, 8, 96, 56}, 1u),
          "the waves a second later");
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "render the moved waves");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.waves_drawn == 3u && stats.waves_rendered == 2u, "a changed command renders again");
    nk_ui_draw_buffer_reset(&buffer);
    for (i = 0; i < 5; ++i) {
        CHECK(nk_ui_draw_add_custom(&buffer, NK_UI_CUSTOM_WAVES, &waves, (NkUiPixelRect){0, 0, 9, 9},
                                    1u), "queue one of five waves");
    }
    CHECK(frame(&target, vk, &buffer.list, &buffer.list, 0x000000FFu, &prepared, &rendered),
          "submit the five-waves frame");
    CHECK(!prepared && !rendered, "more waves than the backend holds textures for fail the frame");
    nk_ui_draw_buffer_reset(&buffer);
    CHECK(nk_ui_draw_add_custom(&buffer, NK_UI_CUSTOM_WAVES, &waves, (NkUiPixelRect){0, 8, 96, 56}, 1u),
          "one waves command again");
    CHECK(draw(&target, vk, &buffer.list, 0x000000FFu), "the backend draws again after the failure");
    CHECK(color_close(target_pixel(&target, 48, 40),
                      ((uint32_t)sample_bilinear(reference, 32, 16, 48.5f / 3.0f - 0.5f,
                                                 32.5f / 3.0f - 0.5f, 0) << 24) |
                      ((uint32_t)sample_bilinear(reference, 32, 16, 48.5f / 3.0f - 0.5f,
                                                 32.5f / 3.0f - 0.5f, 1) << 16) |
                      ((uint32_t)sample_bilinear(reference, 32, 16, 48.5f / 3.0f - 0.5f,
                                                 32.5f / 3.0f - 0.5f, 2) << 8) | 0xFFu, 3),
          "and draws the right frame");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_vk_destroy(vk);
    target_close(&target);
    return 1;
}

/* Fail closed: NULL arguments, a slot out of range, a malformed list or one
   nested too deep are refused and draw nothing; render draws only the list
   prepare took; the backend recovers on the next good frame. */
static int test_fail_closed(void) {
    static const NkUiVertex vertices[4] = {
        {0.0f, 0.0f, 0.0f, 0.0f, 0xFFFFFFFFu}, {8.0f, 0.0f, 0.0f, 0.0f, 0xFFFFFFFFu},
        {8.0f, 8.0f, 0.0f, 0.0f, 0xFFFFFFFFu}, {0.0f, 8.0f, 0.0f, 0.0f, 0xFFFFFFFFu},
    };
    static const uint32_t good_indices[6] = {0, 1, 2, 0, 2, 3};
    static const uint32_t bad_indices[6] = {0, 1, 2, 0, 2, 4};
    NkUiDrawCmd commands[NK_UI_DRAW_CLIP_DEPTH + 3u];
    NkUiDrawList list;
    NkUiDrawList other;
    Target target;
    NkUiVkStats stats;
    NkUiVk *vk;
    bool prepared = true;
    bool rendered = true;
    uint32_t i;
    CHECK(target_open(&target, 16, 16), "open a 16 x 16 target");
    vk = backend_create(NULL);
    CHECK(vk != NULL, "create the backend");
    memset(commands, 0, sizeof(commands));
    commands[0].kind = NK_UI_DRAW_RECTANGLE;
    commands[0].count = 6;
    commands[0].clip[2] = 8;
    commands[0].clip[3] = 8;
    memset(&list, 0, sizeof(list));
    list.v = vertices;
    list.nv = 4;
    list.idx = good_indices;
    list.ni = 6;
    list.cmd = commands;
    list.ncmd = 1;

    CHECK(!nk_ui_vk_prepare(NULL, &list, g_gpu.cb, 0) && !nk_ui_vk_prepare(vk, NULL, g_gpu.cb, 0) &&
          !nk_ui_vk_prepare(vk, &list, NULL, 0) && !nk_ui_vk_prepare(vk, &list, g_gpu.cb, FRAMES),
          "NULL arguments and a slot out of range are refused before anything is recorded");
    CHECK(!nk_ui_vk_render(NULL, &list, g_gpu.cb, 16, 16) && !nk_ui_vk_render(vk, NULL, g_gpu.cb, 16, 16) &&
          !nk_ui_vk_render(vk, &list, NULL, 16, 16), "render refuses NULL arguments");
    CHECK(!nk_ui_vk_render(vk, &list, g_gpu.cb, 16, 16), "render refuses a list that was never prepared");

    list.idx = bad_indices;
    CHECK(frame(&target, vk, &list, &list, 0x000000FFu, &prepared, &rendered), "submit a bad-index frame");
    CHECK(!prepared && target_pixel(&target, 4, 4) == 0x000000FFu, "an index past the vertices is refused");
    list.idx = good_indices;
    commands[0].first = 1;
    CHECK(frame(&target, vk, &list, &list, 0x000000FFu, &prepared, &rendered), "submit a bad-range frame");
    CHECK(!prepared, "a command range past the indices is refused");
    commands[0].first = 0;

    /* Scissor nesting: NK_UI_DRAW_CLIP_DEPTH deep is fine, one more is not, and
       an end with none open is malformed. */
    for (i = 1; i <= NK_UI_DRAW_CLIP_DEPTH; ++i) {
        commands[i].kind = NK_UI_DRAW_SCISSOR_START;
        commands[i].clip[2] = 16;
        commands[i].clip[3] = 16;
    }
    list.ncmd = NK_UI_DRAW_CLIP_DEPTH + 1u;
    CHECK(draw(&target, vk, &list, 0x000000FFu), "scissors nested NK_UI_DRAW_CLIP_DEPTH deep");
    commands[NK_UI_DRAW_CLIP_DEPTH + 1u] = commands[1];
    list.ncmd = NK_UI_DRAW_CLIP_DEPTH + 2u;
    CHECK(frame(&target, vk, &list, &list, 0x000000FFu, &prepared, &rendered), "submit the deep frame");
    CHECK(!prepared && target_pixel(&target, 4, 4) == 0x000000FFu, "one level deeper is refused");
    commands[1].kind = NK_UI_DRAW_SCISSOR_END;
    list.ncmd = 2;
    CHECK(frame(&target, vk, &list, &list, 0x000000FFu, &prepared, &rendered), "submit the stray end");
    CHECK(!prepared, "a scissor end with none open is refused");
    CHECK(!nk_ui_vk_render(vk, &list, g_gpu.cb, 16, 16), "render refuses after a failed prepare");

    list.ncmd = 1;
    CHECK(draw(&target, vk, &list, 0x000000FFu), "a good frame after the failures");
    CHECK(target_pixel(&target, 4, 4) == 0xFFFFFFFFu && target_pixel(&target, 8, 8) == 0x000000FFu,
          "it draws");
    other = list;
    CHECK(frame(&target, vk, &list, &other, 0x000000FFu, &prepared, &rendered), "render another list");
    CHECK(prepared && !rendered, "render refuses a list other than the one prepared");
    CHECK(frame(&target, vk, &list, &list, 0x000000FFu, &prepared, &rendered) && prepared && rendered,
          "the prepared list renders");
    nk_ui_vk_get_stats(vk, &stats);
    CHECK(stats.frames_failed == 7u, "every refused prepare is counted");
    nk_ui_vk_destroy(vk);
    target_close(&target);
    return 1;
}

int main(void) {
    char reason[256];
    int passed = 0;
    int total = 0;
    int status = gpu_open(reason, sizeof(reason));
    if (status == 0) {
        printf("SKIP nk_ui_vk: %s\n", reason);
        gpu_close();
        return EXIT_SKIP;
    }
    if (status < 0) {
        printf("FAIL nk_ui_vk: %s\n", reason);
        gpu_close();
        return 1;
    }
    printf("nk_ui_vk: device \"%s\", validation layer %s\n", g_gpu.device_name,
           g_gpu.validation ? "on" : "not installed (or NK_UI_VK_VALIDATION=0)");
#define RUN(test) \
    do { \
        ++total; \
        if (test()) { \
            ++passed; \
            printf("PASS %s\n", #test); \
        } \
    } while (0)
    RUN(test_availability);
    RUN(test_scissor);
    RUN(test_premultiplied_blend);
    RUN(test_rounded_corner_fringe);
    RUN(test_text);
    RUN(test_atlas_page_overflow);
    RUN(test_atlas_rebuild_on_scale_change);
    RUN(test_image_commands);
    RUN(test_image_cache_overflow);
    RUN(test_waves);
    RUN(test_fail_closed);
#undef RUN
    gpu_close();
    if (g_validation_errors > 0) {
        printf("FAIL nk_ui_vk: the validation layer reported %d error(s)\n", g_validation_errors);
    }
    printf("nk_ui_vk: %d of %d tests passed\n", passed, total);
    return g_failures == 0 && passed == total && g_validation_errors == 0 ? 0 : 1;
}

#endif /* NK_UI_WITH_VULKAN */
