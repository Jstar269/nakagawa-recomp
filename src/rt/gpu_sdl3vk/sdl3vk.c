// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.
//
/*
 * SDL3 owns the window/input path and Vulkan owns the swapchain presentation path. The
 * module accepts both software-rendered guest frames and images produced by ge_gpu.c.
 *
 * Presentation uses a small frame ring.  The CPU waits only when reusing that frame's
 * resources; vkQueuePresentKHR is otherwise non-blocking.
 *
 * AI disclosure: this renderer is an original implementation (it does not reuse PPSSPP's
 * GPU) written with substantial assistance from an LLM (Anthropic Claude). See NOTICE.md.
 * GPLv2+: it consumes ge.c, whose GE semantics are derived from PPSSPP. */

#include "sdl3vk.h"
#include "../perf.h"
#include "../fbcap_policy.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <vulkan/vulkan.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#endif

#define PSP_W 480
#define PSP_H 272
#define PRESENT_FRAMES 3

typedef struct PresentFrame {
    VkCommandBuffer cmd;
    VkFence fence;
    VkSemaphore sem_acq, sem_done;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    void *staging_map;
    VkImage fbimg;
    VkDeviceMemory fbimg_mem;
    VkImage source;
    int submitted;
} PresentFrame;

static SDL_Window      *s_win;
static SDL_Gamepad     *s_pad;
static VkInstance       s_inst;
static VkSurfaceKHR     s_surf;
static VkPhysicalDevice s_pdev;
static VkDevice         s_dev;
static uint32_t         s_qfam;
static VkQueue          s_queue;
static VkCommandPool    s_pool;
static VkCommandBuffer  s_cmd;              /* diagnostic capture only */
static VkFence          s_fence;
static PresentFrame     s_frame[PRESENT_FRAMES];
static uint32_t         s_frame_cursor;

static VkSwapchainKHR s_swap;
static VkFormat       s_swap_fmt;
static VkExtent2D     s_swap_ext;
static uint32_t       s_swap_n;
static VkImage        s_swap_img[8];
static VkFence        s_swap_img_fence[8];

static int      s_renderer_terminal = 0;
static uint64_t s_swapchain_gen = 0;
static uint64_t s_frame_sem_gen = 0;

static int     s_present_fault_armed = 0;
static VkResult s_present_fault_result = VK_SUCCESS;

int sdl3vk_renderer_terminal(void) {
    return s_renderer_terminal;
}

void sdl3vk_present_fault_inject(int vk_result) {
    s_present_fault_armed = 1;
    s_present_fault_result = (VkResult)vk_result;
}

void sdl3vk_present_fault_clear(void) {
    s_present_fault_armed = 0;
    s_present_fault_result = VK_SUCCESS;
}

uint64_t sdl3vk_swapchain_generation(void) {
    return s_swapchain_gen;
}

uint64_t sdl3vk_frame_semaphore_generation(void) {
    return s_frame_sem_gen;
}

static uint32_t s_buttons;
static uint8_t  s_lx = 128, s_ly = 128;
static int      s_pad_present;

/* ---- validation layer (issue #57) --------------------------------------------------- */

static int         s_validation_on;      /* SR_VULKAN_VALIDATION requested AND enabled */
static int         s_validation_errors;  /* ERROR-severity messages since init */
static VkDebugUtilsMessengerEXT s_validation_msgr;
static PFN_vkDestroyDebugUtilsMessengerEXT s_validation_destroy;

static VKAPI_ATTR VkBool32 VKAPI_CALL s_validation_cb(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT *data, void *user) {
    (void)type; (void)user;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) s_validation_errors++;
    fprintf(stderr, "[VulkanValidation] %s: %s\n",
            (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "ERROR" : "WARNING",
            data && data->pMessage ? data->pMessage : "(no message)");
    return VK_FALSE;
}

int sdl3vk_validation_error_count(void) { return s_validation_errors; }

#define VK_TRY(expr) do { VkResult vr_ = (expr); if (vr_ != VK_SUCCESS) { \
    fprintf(stderr, "sdl3vk: %s failed: %d\n", #expr, (int)vr_); return 0; } } while (0)

static uint32_t find_mem_type(uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(s_pdev, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int wait_present_frame(PresentFrame *f) {
    if (!f->submitted) return 1;
    uint64_t wait_started = sr_perf_now_ns();
    VkResult r=vkWaitForFences(s_dev,1,&f->fence,VK_TRUE,UINT64_MAX);
    sr_perf_present_wait(wait_started);
    if(r!=VK_SUCCESS)return 0;
    f->submitted=0;
    f->source=VK_NULL_HANDLE;
    return 1;
}

static int wait_all_present_frames(void) {
    for(int i=0;i<PRESENT_FRAMES;i++)if(!wait_present_frame(&s_frame[i]))return 0;
    return 1;
}

/* ---- swapchain ---------------------------------------------------------------------- */

static void destroy_swapchain(void) {
    if (s_swap) { vkDestroySwapchainKHR(s_dev, s_swap, NULL); s_swap = VK_NULL_HANDLE; }
}

static int create_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    VK_TRY(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_pdev, s_surf, &caps));

    /* Prefer BGRA8 UNORM (matches the framebuffer byte order); fall back to whatever the
     * surface offers first. */
    VkSurfaceFormatKHR fmts[32]; uint32_t nf = 32;
    VK_TRY(vkGetPhysicalDeviceSurfaceFormatsKHR(s_pdev, s_surf, &nf, fmts));
    VkSurfaceFormatKHR pick = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM) { pick = fmts[i]; break; }
    s_swap_fmt = pick.format;

    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {   /* surface lets us choose: use the window pixel size */
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(s_win, &w, &h);
        ext.width  = (uint32_t)w;  ext.height = (uint32_t)h;
    }
    if (ext.width  < caps.minImageExtent.width)  ext.width  = caps.minImageExtent.width;
    if (ext.width  > caps.maxImageExtent.width)  ext.width  = caps.maxImageExtent.width;
    if (ext.height < caps.minImageExtent.height) ext.height = caps.minImageExtent.height;
    if (ext.height > caps.maxImageExtent.height) ext.height = caps.maxImageExtent.height;
    if (ext.width == 0 || ext.height == 0) return 0;   /* minimized: keep old swapchain */
    s_swap_ext = ext;

    uint32_t n = caps.minImageCount + 1;
    if (caps.maxImageCount && n > caps.maxImageCount) n = caps.maxImageCount;

    VkSwapchainKHR old = s_swap;
    VkSwapchainCreateInfoKHR sci = { .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    sci.surface          = s_surf;
    sci.minImageCount    = n;
    sci.imageFormat      = pick.format;
    sci.imageColorSpace  = pick.colorSpace;
    sci.imageExtent      = ext;
    sci.imageArrayLayers = 1;
    sci.imageUsage       = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform     = caps.currentTransform;
    sci.compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    /* Game speed is paced by the scheduler's vblank clock (sched.c vblank_pace), not by
     * presentation. MAILBOX (no tearing, never blocks) avoids beating against the monitor
     * refresh; FIFO is the mandated fallback. */
    sci.presentMode      = VK_PRESENT_MODE_FIFO_KHR;
    {
        VkPresentModeKHR pm[8]; uint32_t npm = 8;
        if (vkGetPhysicalDeviceSurfacePresentModesKHR(s_pdev, s_surf, &npm, pm) >= VK_SUCCESS)
            for (uint32_t i = 0; i < npm; i++)
                if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR) { sci.presentMode = pm[i]; break; }
    }
    sci.clipped          = VK_TRUE;
    sci.oldSwapchain     = old;
    VK_TRY(vkCreateSwapchainKHR(s_dev, &sci, NULL, &s_swap));
    if (old) vkDestroySwapchainKHR(s_dev, old, NULL);

    uint32_t n_img = (uint32_t)(sizeof(s_swap_img) / sizeof(s_swap_img[0]));
    VK_TRY(vkGetSwapchainImagesKHR(s_dev, s_swap, &n_img, s_swap_img));
    s_swap_n = n_img;
    memset(s_swap_img_fence,0,sizeof(s_swap_img_fence));
    s_swapchain_gen++;
    return 1;
}

/* ---- init --------------------------------------------------------------------------- */

int sdl3vk_init(const char *title) {
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "sdl3vk: SDL_Init failed: %s\n", SDL_GetError());
        return 0;
    }
    s_win = SDL_CreateWindow(title ? title : "Nakagawa Recomp",
                             PSP_W * 2, PSP_H * 2,
                             SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE);
    if (!s_win) {
        fprintf(stderr, "sdl3vk: SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 0;
    }

    Uint32 next = 0;
    {
        const char *val = getenv("SR_VULKAN_VALIDATION");
        s_validation_on = val && val[0] && atoi(val) != 0;
    }
    const char *sdl_ext[16];
    {
        const char *const *sdl = SDL_Vulkan_GetInstanceExtensions(&next);
        if (next > 16) next = 16;
        for (uint32_t i = 0; i < next; i++) sdl_ext[i] = sdl[i];
    }
    const char *layer_names[1];
    uint32_t n_layers = 0;
    if (s_validation_on) {
        /* Fail closed when the layer was requested but is not installed: a silently
         * missing layer would make a "validation-clean" evidence claim meaningless. */
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, NULL);
        VkLayerProperties lp[64];
        int have = 0;
        if (n > 64) n = 64;
        if (vkEnumerateInstanceLayerProperties(&n, lp) == VK_SUCCESS)
            for (uint32_t i = 0; i < n; i++)
                if (strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) { have = 1; break; }
        if (!have) {
            fprintf(stderr, "sdl3vk: SR_VULKAN_VALIDATION set but VK_LAYER_KHRONOS_validation "
                            "is not installed; refusing to initialize\n");
            return 0;
        }
        if (next >= 16) {
            fprintf(stderr, "sdl3vk: too many instance extensions to enable VK_EXT_debug_utils\n");
            return 0;
        }
        sdl_ext[next++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
        layer_names[0] = "VK_LAYER_KHRONOS_validation";
        n_layers = 1;
    }
    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO };
    ai.pApplicationName = "psp_recomp";
    ai.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    ici.pApplicationInfo = &ai;
    ici.enabledExtensionCount = next;
    ici.ppEnabledExtensionNames = sdl_ext;
    ici.enabledLayerCount = n_layers;
    ici.ppEnabledLayerNames = n_layers ? layer_names : NULL;
    VK_TRY(vkCreateInstance(&ici, NULL, &s_inst));
    if (s_validation_on) {
        PFN_vkCreateDebugUtilsMessengerEXT create_msgr = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(s_inst, "vkCreateDebugUtilsMessengerEXT");
        s_validation_destroy = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(s_inst, "vkDestroyDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mci =
            { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT };
        mci.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                              VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
        mci.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                          VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
        mci.pfnUserCallback = s_validation_cb;
        if (!create_msgr ||
            create_msgr(s_inst, &mci, NULL, &s_validation_msgr) != VK_SUCCESS) {
            fprintf(stderr, "sdl3vk: could not create the validation messenger\n");
            return 0;
        }
    }

    if (!SDL_Vulkan_CreateSurface(s_win, s_inst, NULL, &s_surf)) {
        fprintf(stderr, "sdl3vk: SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
        return 0;
    }

    /* Physical device: first one with a graphics queue that can present to our surface. */
    VkPhysicalDevice devs[16]; uint32_t nd = 16;
    VK_TRY(vkEnumeratePhysicalDevices(s_inst, &nd, devs));
    s_pdev = VK_NULL_HANDLE;
    for (uint32_t d = 0; d < nd && !s_pdev; d++) {
        VkQueueFamilyProperties qf[16]; uint32_t nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, qf);
        for (uint32_t q = 0; q < nq; q++) {
            VkBool32 can_present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], q, s_surf, &can_present);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && can_present) {
                s_pdev = devs[d]; s_qfam = q; break;
            }
        }
    }
    if (!s_pdev) { fprintf(stderr, "sdl3vk: no usable Vulkan device\n"); return 0; }
    {
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(s_pdev, &pp);
        fprintf(stderr, "sdl3vk: using %s\n", pp.deviceName);
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    qci.queueFamilyIndex = s_qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char *dev_ext[] = { VK_KHR_SWAPCHAIN_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dev_ext;
    VK_TRY(vkCreateDevice(s_pdev, &dci, NULL, &s_dev));
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);

    VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = s_qfam;
    VK_TRY(vkCreateCommandPool(s_dev, &cpi, NULL, &s_pool));
    VkCommandBufferAllocateInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    cbi.commandPool = s_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VK_TRY(vkAllocateCommandBuffers(s_dev, &cbi, &s_cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VK_TRY(vkCreateFence(s_dev, &fci, NULL, &s_fence));
    VkSemaphoreCreateInfo sci2 = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    /* Per-frame command/sync/upload resources.  No resource is reused before its fence. */
    for(int i=0;i<PRESENT_FRAMES;i++){
        PresentFrame *f=&s_frame[i];
        cbi.commandBufferCount=1;
        VK_TRY(vkAllocateCommandBuffers(s_dev,&cbi,&f->cmd));
        VK_TRY(vkCreateFence(s_dev,&fci,NULL,&f->fence));
        VK_TRY(vkCreateSemaphore(s_dev,&sci2,NULL,&f->sem_acq));
        VK_TRY(vkCreateSemaphore(s_dev,&sci2,NULL,&f->sem_done));

        VkBufferCreateInfo bci={.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size=PSP_W*PSP_H*4;bci.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VK_TRY(vkCreateBuffer(s_dev,&bci,NULL,&f->staging));
        VkMemoryRequirements mr;vkGetBufferMemoryRequirements(s_dev,f->staging,&mr);
        VkMemoryAllocateInfo mai={.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        mai.allocationSize=mr.size;
        mai.memoryTypeIndex=find_mem_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        VK_TRY(vkAllocateMemory(s_dev,&mai,NULL,&f->staging_mem));
        VK_TRY(vkBindBufferMemory(s_dev,f->staging,f->staging_mem,0));
        VK_TRY(vkMapMemory(s_dev,f->staging_mem,0,VK_WHOLE_SIZE,0,&f->staging_map));

        VkImageCreateInfo imi={.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        imi.imageType=VK_IMAGE_TYPE_2D;imi.format=VK_FORMAT_B8G8R8A8_UNORM;
        imi.extent.width=PSP_W;imi.extent.height=PSP_H;imi.extent.depth=1;
        imi.mipLevels=1;imi.arrayLayers=1;imi.samples=VK_SAMPLE_COUNT_1_BIT;
        imi.tiling=VK_IMAGE_TILING_OPTIMAL;
        imi.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        imi.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        VK_TRY(vkCreateImage(s_dev,&imi,NULL,&f->fbimg));
        vkGetImageMemoryRequirements(s_dev,f->fbimg,&mr);
        mai.allocationSize=mr.size;
        mai.memoryTypeIndex=find_mem_type(mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_TRY(vkAllocateMemory(s_dev,&mai,NULL,&f->fbimg_mem));
        VK_TRY(vkBindImageMemory(s_dev,f->fbimg,f->fbimg_mem,0));
    }

    if (!create_swapchain()) return 0;

    if (SDL_HasGamepad()) {
        int npads = 0;
        SDL_JoystickID *ids = SDL_GetGamepads(&npads);
        if (ids && npads > 0) s_pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }
    fprintf(stderr, "sdl3vk: init ok (%ux%u swapchain, fmt %d, gamepad=%s)\n",
            s_swap_ext.width, s_swap_ext.height, (int)s_swap_fmt, s_pad ? "yes" : "no");
    return 1;
}

/* ---- input -------------------------------------------------------------------------- */

/* Rendering can be slower than the host input event rate while translated code is still
 * unoptimised.  A complete press+release may therefore be queued between two presents; polling
 * SDL_GetKeyboardState()/SDL_GetGamepadButton() alone loses that edge.  Preserve button-down
 * events until the next published PSP sample, while the ordinary state polls continue to model
 * held buttons and axes. */
static uint32_t s_button_pulse;

static uint32_t keyboard_button(SDL_Scancode sc) {
    switch (sc) {
    case SDL_SCANCODE_RETURN: return 0x0008;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return 0x0001;
    case SDL_SCANCODE_X:      return 0x4000;
    case SDL_SCANCODE_Z:      return 0x2000;
    case SDL_SCANCODE_A:      return 0x8000;
    case SDL_SCANCODE_S:      return 0x1000;
    case SDL_SCANCODE_Q:      return 0x0100;
    case SDL_SCANCODE_W:      return 0x0200;
    case SDL_SCANCODE_UP:     return 0x0010;
    case SDL_SCANCODE_DOWN:   return 0x0040;
    case SDL_SCANCODE_LEFT:   return 0x0080;
    case SDL_SCANCODE_RIGHT:  return 0x0020;
    default:                  return 0;
    }
}

static uint32_t gamepad_button(Uint8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:          return 0x4000;
    case SDL_GAMEPAD_BUTTON_EAST:           return 0x2000;
    case SDL_GAMEPAD_BUTTON_WEST:           return 0x8000;
    case SDL_GAMEPAD_BUTTON_NORTH:          return 0x1000;
    case SDL_GAMEPAD_BUTTON_START:          return 0x0008;
    case SDL_GAMEPAD_BUTTON_BACK:           return 0x0001;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:  return 0x0100;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return 0x0200;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:        return 0x0010;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:      return 0x0040;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:      return 0x0080;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:     return 0x0020;
    default:                                return 0;
    }
}

static void poll_input(int *quit) {
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        switch (ev.type) {
        case SDL_EVENT_QUIT: *quit = 1; break;
        case SDL_EVENT_KEY_DOWN:
            if (ev.key.key == SDLK_ESCAPE) *quit = 1;
            if (!ev.key.repeat) s_button_pulse |= keyboard_button(ev.key.scancode);
            break;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            s_button_pulse |= gamepad_button(ev.gbutton.button);
            break;
        case SDL_EVENT_GAMEPAD_ADDED:
            if (!s_pad) s_pad = SDL_OpenGamepad(ev.gdevice.which);
            break;
        case SDL_EVENT_GAMEPAD_REMOVED:
            if (s_pad && SDL_GetGamepadID(s_pad) == ev.gdevice.which) {
                SDL_CloseGamepad(s_pad); s_pad = NULL;
            }
            break;
        default: break;
        }
    }

    uint32_t b = 0;
    const bool *k = SDL_GetKeyboardState(NULL);
    /* Same bindings as the GDI front-end (gui.c read_keys). */
    if (k[SDL_SCANCODE_RETURN]) b |= 0x0008;                       /* START   */
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT]) b |= 0x0001; /* SELECT */
    if (k[SDL_SCANCODE_X]) b |= 0x4000;                            /* CROSS   */
    if (k[SDL_SCANCODE_Z]) b |= 0x2000;                            /* CIRCLE  */
    if (k[SDL_SCANCODE_A]) b |= 0x8000;                            /* SQUARE  */
    if (k[SDL_SCANCODE_S]) b |= 0x1000;                            /* TRIANGLE*/
    if (k[SDL_SCANCODE_Q]) b |= 0x0100;                            /* L       */
    if (k[SDL_SCANCODE_W]) b |= 0x0200;                            /* R       */
    if (k[SDL_SCANCODE_UP])    b |= 0x0010;
    if (k[SDL_SCANCODE_DOWN])  b |= 0x0040;
    if (k[SDL_SCANCODE_LEFT])  b |= 0x0080;
    if (k[SDL_SCANCODE_RIGHT]) b |= 0x0020;

    uint8_t lx = 128, ly = 128;
    s_pad_present = s_pad != NULL;
    if (s_pad) {
        #define PB(sdlb, bit) do { if (SDL_GetGamepadButton(s_pad, sdlb)) b |= (bit); } while (0)
        PB(SDL_GAMEPAD_BUTTON_SOUTH, 0x4000);          /* CROSS    */
        PB(SDL_GAMEPAD_BUTTON_EAST,  0x2000);          /* CIRCLE   */
        PB(SDL_GAMEPAD_BUTTON_WEST,  0x8000);          /* SQUARE   */
        PB(SDL_GAMEPAD_BUTTON_NORTH, 0x1000);          /* TRIANGLE */
        PB(SDL_GAMEPAD_BUTTON_START, 0x0008);
        PB(SDL_GAMEPAD_BUTTON_BACK,  0x0001);
        PB(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  0x0100);
        PB(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 0x0200);
        PB(SDL_GAMEPAD_BUTTON_DPAD_UP,    0x0010);
        PB(SDL_GAMEPAD_BUTTON_DPAD_DOWN,  0x0040);
        PB(SDL_GAMEPAD_BUTTON_DPAD_LEFT,  0x0080);
        PB(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, 0x0020);
        #undef PB
        if (SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER)  > 8192) b |= 0x0100;
        if (SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 8192) b |= 0x0200;
        int ax = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTX);
        int ay = SDL_GetGamepadAxis(s_pad, SDL_GAMEPAD_AXIS_LEFTY);
        if (ax < -7849 || ax > 7849) lx = (uint8_t)((ax + 32768) * 255 / 65535);
        if (ay < -7849 || ay > 7849) ly = (uint8_t)((ay + 32768) * 255 / 65535);
    }
    uint32_t published = b | s_button_pulse;
    if (getenv("SR_INLOG")) {
        static uint32_t previous_published;
        if (published != previous_published)
        fprintf(stderr, "sdl_input: buttons 0x%04x -> 0x%04x pad=%d lx=%u ly=%u\n",
                previous_published, published, s_pad != NULL, lx, ly);
        previous_published = published;
    }
    s_buttons = b;
    s_lx = lx; s_ly = ly;
}

int sdl3vk_get_vk(Sdl3VkInfo *out) {
    if (!s_dev || !out) return 0;
    out->instance = (void *)s_inst;
    out->physical = (void *)s_pdev;
    out->device   = (void *)s_dev;
    out->queue    = (void *)s_queue;
    out->queue_family = s_qfam;
    return 1;
}

uint32_t sdl3vk_buttons(void) { return s_buttons | s_button_pulse; }
void sdl3vk_consume_button_pulses(void) { s_button_pulse = 0; }
void sdl3vk_analog(uint8_t *lx, uint8_t *ly) { if (lx) *lx = s_lx; if (ly) *ly = s_ly; }
int  sdl3vk_pad_present(void) { return s_pad_present; }

/* ---- present ------------------------------------------------------------------------ */

static void barrier(VkCommandBuffer cmd, VkImage img,
                    VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src_acc, VkAccessFlags dst_acc,
                    VkPipelineStageFlags src_st, VkPipelineStageFlags dst_st) {
    VkImageMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    mb.srcAccessMask = src_acc;
    mb.dstAccessMask = dst_acc;
    mb.oldLayout = from;
    mb.newLayout = to;
    mb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    mb.image = img;
    mb.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    mb.subresourceRange.levelCount = 1;
    mb.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, src_st, dst_st, 0, 0, NULL, 0, NULL, 1, &mb);
}

/* Common present: blit `src` (already TRANSFER_SRC_OPTIMAL; srcw x srch source region)
 * onto the swapchain with aspect-correct letterboxing. upload!=NULL additionally
 * records the staging->s_fbimg copy first (the CPU framebuffer path). */
int sdl3vk_poll(void) {
    int quit = 0;
    poll_input(&quit);
    return quit ? 0 : 1;
}

/* ---- present-source capture (issue #57) --------------------------------------------- */

enum {
    CAP_IDLE = 0,      /* nothing armed */
    CAP_ARMED,         /* armed; the next presented frame will be recorded */
    CAP_RECORDED,      /* recorded in a submitted present; waiting on the frame fence */
    CAP_DONE,          /* file published */
    CAP_FAILED         /* attempted and failed */
};

enum {
    CAP_SRC_NONE = 0,
    CAP_SRC_CPU,       /* sdl3vk_present_rgba: fbimg is B8G8R8A8_UNORM */
    CAP_SRC_GPU        /* sdl3vk_present_image(_ex): GE target is R8G8B8A8_UNORM */
};

static int      s_cap_state = CAP_IDLE;
static int      s_cap_result;             /* 1 written, 0 nothing attempted, -1 failed */
static char     s_cap_path[1024];
static int      s_cap_src_kind;
static uint32_t s_cap_w, s_cap_h;
static VkFormat s_cap_fmt;
static VkBuffer s_cap_buf;
static VkDeviceMemory s_cap_mem;
static void    *s_cap_map;
static uint64_t s_cap_alloc;              /* allocated byte capacity (grows on demand) */
static int      s_cap_noncoherent;        /* memory type lacks HOST_COHERENT */

static void cap_free_buffer(void) {
    if (s_cap_map) vkUnmapMemory(s_dev, s_cap_mem);
    s_cap_map = NULL;
    if (s_cap_buf) vkDestroyBuffer(s_dev, s_cap_buf, NULL);
    s_cap_buf = VK_NULL_HANDLE;
    if (s_cap_mem) vkFreeMemory(s_dev, s_cap_mem, NULL);
    s_cap_mem = VK_NULL_HANDLE;
    s_cap_alloc = 0;
    s_cap_noncoherent = 0;
}

/* The readback buffer is allocated from THIS buffer's own VkBufferMemoryRequirements
 * (size/alignment/memory-type bits) -- never from another object's requirements -- and is
 * grown lazily. HOST_COHERENT is preferred; on a non-coherent type the host must
 * invalidate the mapped range after the copy fence completes (cap_write_file). */
static int cap_ensure(uint32_t w, uint32_t h) {
    uint64_t need = (uint64_t)w * (uint64_t)h * 4u;
    if (need == 0 || need > UINT32_MAX) return 0;
    if (s_cap_buf && need <= s_cap_alloc) return 1;
    cap_free_buffer();
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = (VkDeviceSize)need;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &s_cap_buf) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(s_dev, s_cap_buf, &mr);
    if (need > mr.size) { cap_free_buffer(); return 0; }
    uint32_t mtype = find_mem_type(mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                   VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    s_cap_noncoherent = 0;
    if (mtype == UINT32_MAX) {
        mtype = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        s_cap_noncoherent = 1;
    }
    if (mtype == UINT32_MAX) { cap_free_buffer(); return 0; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = mtype;
    if (vkAllocateMemory(s_dev, &mai, NULL, &s_cap_mem) != VK_SUCCESS ||
        vkBindBufferMemory(s_dev, s_cap_buf, s_cap_mem, 0) != VK_SUCCESS ||
        vkMapMemory(s_dev, s_cap_mem, 0, VK_WHOLE_SIZE, 0, &s_cap_map) != VK_SUCCESS) {
        cap_free_buffer();
        return 0;
    }
    s_cap_alloc = need;
    return 1;
}

/* Recorded in the SAME command buffer that presents the frame, after the present blit.
 * The source image is still TRANSFER_SRC_OPTIMAL -- its content is what the presentation
 * engine receives -- so no layout transition is needed and nothing is discarded. The
 * buffer rows are tightly packed (no bufferRowLength), so the mapped pitch is w*4. */
static int cap_record(VkCommandBuffer cmd, VkImage src, int srcw, int srch) {
    if (!src || !s_cap_buf) return 0;
    VkBufferImageCopy bic = {0};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = (uint32_t)srcw;
    bic.imageExtent.height = (uint32_t)srch;
    bic.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           s_cap_buf, 1, &bic);
    VkBufferMemoryBarrier bb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    bb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    bb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bb.buffer = s_cap_buf;
    bb.offset = 0;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                         0, 0, NULL, 1, &bb, 0, NULL);
    return 1;
}

/* Publish the readback as a P6 PPM whose name ends in .ppm: the format matches the
 * extension. Rows are read at the copy's tight pitch (w*4 bytes), not at some
 * allocation-derived pitch, and exactly w*h*3 bytes are written. Publication is atomic:
 * a temp sibling file is renamed over the destination only after a complete write, so a
 * reader never observes a half-written file. */
static int cap_write_file(void) {
    if (!s_cap_buf || !s_cap_map || !s_cap_path[0]) return 0;
    if (s_cap_noncoherent) {
        VkMappedMemoryRange rng = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE };
        rng.memory = s_cap_mem;
        rng.offset = 0;
        rng.size = VK_WHOLE_SIZE;
        if (vkInvalidateMappedMemoryRanges(s_dev, 1, &rng) != VK_SUCCESS) return 0;
    }
    static unsigned long s_cap_tmp_seq;
    char tmp[1024 + 64];
    if (snprintf(tmp, sizeof tmp, "%s.tmp%lu", s_cap_path,
                 ++s_cap_tmp_seq) >= (int)sizeof tmp)
        return 0;
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    int ok = 0;
    if (fprintf(f, "P6\n%u %u\n255\n", s_cap_w, s_cap_h) > 0) {
        const uint8_t *px = (const uint8_t *)s_cap_map;
        uint32_t row = s_cap_w * 4u;             /* tight copy pitch */
        ok = 1;
        if (s_cap_fmt == VK_FORMAT_B8G8R8A8_UNORM) {
            for (uint32_t y = 0; y < s_cap_h && ok; y++)
                for (uint32_t x = 0; x < s_cap_w; x++) {
                    const uint8_t *p = px + (size_t)y * row + x * 4u;
                    if (fputc(p[2], f) == EOF || fputc(p[1], f) == EOF ||
                        fputc(p[0], f) == EOF) { ok = 0; break; }
                }
        } else if (s_cap_fmt == VK_FORMAT_R8G8B8A8_UNORM) {
            for (uint32_t y = 0; y < s_cap_h && ok; y++)
                for (uint32_t x = 0; x < s_cap_w; x++) {
                    const uint8_t *p = px + (size_t)y * row + x * 4u;
                    if (fputc(p[0], f) == EOF || fputc(p[1], f) == EOF ||
                        fputc(p[2], f) == EOF) { ok = 0; break; }
                }
        } else {
            ok = 0;   /* unknown source format: refuse to guess the channel order */
        }
    }
    if (ok && fflush(f) != 0) ok = 0;
    if (fclose(f) != 0) ok = 0;
    if (!ok) { remove(tmp); return 0; }
#ifdef _WIN32
    if (!MoveFileExA(tmp, s_cap_path, MOVEFILE_REPLACE_EXISTING)) {
        remove(tmp);
        fprintf(stderr, "sdl3vk: capture publish failed (MoveFileExA: %lu)\n",
                (unsigned long)GetLastError());
        return 0;
    }
#else
    if (rename(tmp, s_cap_path) != 0) { remove(tmp); return 0; }
#endif
    return 1;
}

/* The single terminal transition out of the armed/recorded states. Every present-path
 * failure funnels through here so sdl3vk_capture_result() can never report a stale or
 * invented outcome. */
static void cap_finish(int ok, const char *why) {
    if (s_cap_state != CAP_ARMED && s_cap_state != CAP_RECORDED) return;
    if (ok) {
        s_cap_state = CAP_DONE;
        s_cap_result = 1;
    } else {
        s_cap_state = CAP_FAILED;
        s_cap_result = -1;
        fprintf(stderr, "sdl3vk: present capture failed: %s\n",
                why ? why : "unknown reason");
    }
}

int sdl3vk_capture_arm(const char *path) {
    if (s_renderer_terminal) return 0;
    if (!s_dev || !s_swap) return 0;
    if (s_cap_state != CAP_IDLE && s_cap_state != CAP_DONE && s_cap_state != CAP_FAILED)
        return 0;
    if (!path || !path[0] || strlen(path) >= sizeof s_cap_path) return 0;
    strcpy(s_cap_path, path);
    s_cap_result = 0;
    s_cap_src_kind = CAP_SRC_NONE;
    s_cap_w = s_cap_h = 0;
    s_cap_fmt = VK_FORMAT_UNDEFINED;
    s_cap_state = CAP_ARMED;
    return 1;
}

int sdl3vk_capture_result(void) { return s_cap_result; }

void sdl3vk_capture_cancel(void) {
    if (s_cap_state != CAP_ARMED) return;
    s_cap_state = CAP_IDLE;
    s_cap_path[0] = '\0';
    s_cap_result = 0;
    cap_free_buffer();
}

const char *sdl3vk_capture_source_label(void) {
    switch (s_cap_src_kind) {
    case CAP_SRC_CPU: return "cpu-framebuffer";
    case CAP_SRC_GPU: return "gpu-render-target";
    default:          return "";
    }
}

typedef enum PresentDisposition {
    PRESENT_OK,
    PRESENT_ENQUEUED_REBUILD,
    PRESENT_OUT_OF_DATE,
    PRESENT_SURFACE_LOST,
    PRESENT_NOT_ENQUEUED,
    PRESENT_TERMINAL,
    PRESENT_UNCLASSIFIED
} PresentDisposition;

static int is_enqueued_present(PresentDisposition disp) {
    return (disp == PRESENT_OK ||
            disp == PRESENT_ENQUEUED_REBUILD ||
            disp == PRESENT_OUT_OF_DATE ||
            disp == PRESENT_SURFACE_LOST);
}

static PresentDisposition classify_present(VkResult pr, int *out_presented, const char **out_why) {
    switch (pr) {
    case VK_SUCCESS:
        *out_presented = 1;
        *out_why = "vkQueuePresentKHR succeeded";
        return PRESENT_OK;

    case VK_SUBOPTIMAL_KHR:
        *out_presented = 1;
        *out_why = "swapchain suboptimal; enqueued rebuild requested";
        return PRESENT_ENQUEUED_REBUILD;

    case VK_ERROR_OUT_OF_DATE_KHR:
        *out_presented = 0;
        *out_why = "swapchain out of date";
        return PRESENT_OUT_OF_DATE;

    case VK_ERROR_SURFACE_LOST_KHR:
        *out_presented = 0;
        *out_why = "vkQueuePresentKHR failed with VK_ERROR_SURFACE_LOST_KHR";
        return PRESENT_SURFACE_LOST;

    case VK_ERROR_OUT_OF_HOST_MEMORY:
        *out_presented = 0;
        *out_why = "vkQueuePresentKHR failed with VK_ERROR_OUT_OF_HOST_MEMORY";
        return PRESENT_NOT_ENQUEUED;

    case VK_ERROR_OUT_OF_DEVICE_MEMORY:
        *out_presented = 0;
        *out_why = "vkQueuePresentKHR failed with VK_ERROR_OUT_OF_DEVICE_MEMORY";
        return PRESENT_NOT_ENQUEUED;

    case VK_ERROR_DEVICE_LOST:
        *out_presented = 0;
        *out_why = "vkQueuePresentKHR failed with VK_ERROR_DEVICE_LOST";
        return PRESENT_TERMINAL;

    default:
        *out_presented = 0;
        *out_why = "vkQueuePresentKHR failed with unclassified error";
        return PRESENT_UNCLASSIFIED;
    }
}

static int recover_unenqueued_present(PresentFrame *f) {
    if (!s_dev) return 0;
    if (f->submitted) {
        if (vkWaitForFences(s_dev, 1, &f->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            s_renderer_terminal = 1;
            return 0;
        }
        f->submitted = 0;
        f->source = VK_NULL_HANDLE;
    }
    if (f->sem_done) {
        vkDestroySemaphore(s_dev, f->sem_done, NULL);
        f->sem_done = VK_NULL_HANDLE;
    }
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (vkCreateSemaphore(s_dev, &sci, NULL, &f->sem_done) != VK_SUCCESS) {
        s_renderer_terminal = 1;
        return 0;
    }
    s_frame_sem_gen++;
    return 1;
}

static int present_common(VkImage src, int srcw, int srch, const uint32_t *upload) {
    if (s_renderer_terminal) return -1;
    const char *why = NULL;
    int quit = 0;
    int presented = 0;
    poll_input(&quit);
    if (quit) { why = "window closed during present"; goto fail_quit; }
    PresentFrame *f = &s_frame[s_frame_cursor];
    if (!wait_present_frame(f)) { why = "present frame fence wait failed"; goto fail; }
    if (upload) { memcpy(f->staging_map, upload, PSP_W * PSP_H * 4); src = f->fbimg; }
    VkCommandBuffer cmd = f->cmd;

    for (int attempt = 0; attempt < 2; attempt++) {
        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(s_dev, s_swap, UINT64_MAX, f->sem_acq,
                                            VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR || ar == VK_SUBOPTIMAL_KHR) {
            wait_all_present_frames();
            vkQueueWaitIdle(s_queue); /* presentation-engine scope; resize path only */
            if (!create_swapchain()) { why = "swapchain recreation failed"; goto fail; }
            continue;
        }
        if (ar != VK_SUCCESS) { why = "swapchain acquire failed"; goto fail; }
        if (s_swap_img_fence[idx] && s_swap_img_fence[idx] != f->fence) {
            uint64_t wait_started = sr_perf_now_ns();
            vkWaitForFences(s_dev, 1, &s_swap_img_fence[idx], VK_TRUE, UINT64_MAX);
            sr_perf_present_wait(wait_started);
        }
        s_swap_img_fence[idx] = f->fence;

        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &bi);

        if (upload) {
            /* staging buffer -> fb image */
            barrier(cmd, f->fbimg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    0, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy bic = {0};
            bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            bic.imageSubresource.layerCount = 1;
            bic.imageExtent.width = PSP_W; bic.imageExtent.height = PSP_H; bic.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(cmd, f->staging, f->fbimg,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
            barrier(cmd, f->fbimg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        }

        /* fb image -> swapchain, aspect-correct letterbox blit. */
        barrier(cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                0, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        {
            VkClearColorValue black = {{0, 0, 0, 1}};
            VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            vkCmdClearColorImage(cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                 &black, 1, &rng);
        }
        barrier(cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        int dw = (int)s_swap_ext.width, dh = (int)s_swap_ext.height;
        int vw = dw, vh = dw * PSP_H / PSP_W;
        if (vh > dh) { vh = dh; vw = dh * PSP_W / PSP_H; }
        int x0 = (dw - vw) / 2, y0 = (dh - vh) / 2;
        VkImageBlit blt = {0};
        blt.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blt.srcSubresource.layerCount = 1;
        blt.srcOffsets[1].x = srcw; blt.srcOffsets[1].y = srch; blt.srcOffsets[1].z = 1;
        blt.dstSubresource = blt.srcSubresource;
        blt.dstOffsets[0].x = x0;      blt.dstOffsets[0].y = y0;
        blt.dstOffsets[1].x = x0 + vw; blt.dstOffsets[1].y = y0 + vh; blt.dstOffsets[1].z = 1;
        blt.dstOffsets[0].z = 0;
        vkCmdBlitImage(cmd, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &blt, VK_FILTER_LINEAR);
        barrier(cmd, s_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

        if (s_cap_state == CAP_ARMED) {
            if (!cap_ensure((uint32_t)srcw, (uint32_t)srch)) {
                why = "capture buffer allocation failed"; goto fail;
            }
            s_cap_w = (uint32_t)srcw; s_cap_h = (uint32_t)srch;
            s_cap_fmt = upload ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM;
            s_cap_src_kind = upload ? CAP_SRC_CPU : CAP_SRC_GPU;
            if (!cap_record(cmd, src, srcw, srch)) { why = "capture record failed"; goto fail; }
        }
        if (vkEndCommandBuffer(cmd) != VK_SUCCESS) { why = "vkEndCommandBuffer failed"; goto fail; }

        VkPipelineStageFlags wait_st = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &f->sem_acq;
        si.pWaitDstStageMask = &wait_st;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &f->sem_done;
        vkResetFences(s_dev, 1, &f->fence);
        if (vkQueueSubmit(s_queue, 1, &si, f->fence) != VK_SUCCESS) {
            why = "vkQueueSubmit failed"; goto fail;
        }
        sr_perf_present_submit();
        f->submitted = 1;
        f->source = upload ? VK_NULL_HANDLE : src;
        if (s_cap_state == CAP_ARMED) s_cap_state = CAP_RECORDED;

        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &f->sem_done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &s_swap;
        pi.pImageIndices = &idx;

        VkResult pr = VK_SUCCESS;
        if (s_present_fault_armed) {
            s_present_fault_armed = 0;
            pr = s_present_fault_result;
            int fake_presented = 0;
            const char *fake_why = NULL;
            PresentDisposition fd = classify_present(pr, &fake_presented, &fake_why);
            if (is_enqueued_present(fd)) {
                VkPipelineStageFlags wstage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
                VkSubmitInfo esi = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
                esi.waitSemaphoreCount = 1;
                esi.pWaitSemaphores = &f->sem_done;
                esi.pWaitDstStageMask = &wstage;
                if (vkQueueSubmit(s_queue, 1, &esi, VK_NULL_HANDLE) != VK_SUCCESS) {
                    fprintf(stderr, "sdl3vk: fault harness could not emulate semaphore wait\n");
                }
            }
        } else {
            pr = vkQueuePresentKHR(s_queue, &pi);
        }

        int did_present = 0;
        const char *disp_why = NULL;
        PresentDisposition disp = classify_present(pr, &did_present, &disp_why);

        switch (disp) {
        case PRESENT_OK:
            presented = 1;
            break;

        case PRESENT_ENQUEUED_REBUILD:
            presented = 1;
            wait_all_present_frames();
            vkQueueWaitIdle(s_queue);
            if (!create_swapchain()) {
                s_renderer_terminal = 1;
                why = "swapchain rebuild after presentation failed";
                goto fail;
            }
            break;

        case PRESENT_OUT_OF_DATE:
            presented = 0;
            wait_all_present_frames();
            vkQueueWaitIdle(s_queue);
            create_swapchain();
            why = disp_why;
            goto fail;

        case PRESENT_SURFACE_LOST:
            presented = 0;
            s_renderer_terminal = 1;
            /* Surface is lost: present request was enqueued (semaphore waited), but surface is gone.
             * Do NOT call recover_unenqueued_present because f->sem_done WAS consumed. */
            why = disp_why;
            goto fail;

        case PRESENT_NOT_ENQUEUED:
            presented = 0;
            recover_unenqueued_present(f);
            why = disp_why;
            goto fail;

        case PRESENT_UNCLASSIFIED:
            presented = 0;
            s_renderer_terminal = 1;
            /* Unclassified result: queue and wait-semaphore state are unknown.
             * Latch terminal state and do NOT perform per-frame semaphore recovery. */
            why = disp_why;
            goto fail;

        case PRESENT_TERMINAL:
            presented = 0;
            s_renderer_terminal = 1;
            /* Device lost: leaves affected execution state unreliable.
             * Latch terminal state and do NOT attempt normal per-frame renderer recovery. */
            why = disp_why;
            goto fail;
        }

        if (!presented) { why = disp_why; goto fail; }

        s_frame_cursor = (s_frame_cursor + 1) % PRESENT_FRAMES;
        break;
    }
    if (!presented) { why = "swapchain still out of date after recreation"; goto fail; }

    /* Complete the armed capture only now that this frame is known to have reached the
     * presentation engine, so a published file always corresponds to a presented frame.
     * The fence covers this submission; presentation itself stays asynchronous. */
    if (s_cap_state == CAP_RECORDED) {
        if (f->submitted) {
            if (vkWaitForFences(s_dev, 1, &f->fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
                why = "capture fence wait failed"; goto fail;
            }
            f->submitted = 0;
            f->source = VK_NULL_HANDLE;
        }
        if (!cap_write_file()) { why = "capture readback or publication failed"; goto fail; }
        cap_finish(1, NULL);
    }
    return 1;

fail:
    /* One exit for every failure above: the armed capture resolves as failed, never as
     * an invented success. */
    cap_finish(0, why);
    return -1;
fail_quit:
    cap_finish(0, why);
    return 0;
}

int sdl3vk_present_rgba(const uint32_t *px) {
    uint64_t started = sr_perf_now_ns();
    int result = present_common(VK_NULL_HANDLE, PSP_W, PSP_H, px);
    sr_perf_present_done(started, result);
    return result;
}

int sdl3vk_present_image(void *vk_image) {
    /* 512x272 GE target: only the visible 480x272 region is shown */
    uint64_t started = sr_perf_now_ns();
    int result = present_common((VkImage)vk_image, PSP_W, PSP_H, NULL);
    sr_perf_present_done(started, result);
    return result;
}

int sdl3vk_present_image_ex(void *vk_image, int srcw, int srch) {
    /* render-scaled GE target: the visible region scales with the internal resolution */
    uint64_t started = sr_perf_now_ns();
    int result = present_common((VkImage)vk_image, srcw, srch, NULL);
    sr_perf_present_done(started, result);
    return result;
}

int sdl3vk_wait_image(void *vk_image) {
    VkImage image=(VkImage)vk_image;
    if(!image)return 1;
    for(int i=0;i<PRESENT_FRAMES;i++)
        if(s_frame[i].submitted&&s_frame[i].source==image&&!wait_present_frame(&s_frame[i]))
            return 0;
    return 1;
}

/* ---- capture selftest (issue #57) ---------------------------------------------------- */

static int cap_test_pattern(int x, int y, int ch) {
    /* Deterministic per-channel pattern across the whole 8-bit range; every channel
     * varies on both axes, so row-pitch shears and channel-order mistakes change bytes. */
    switch (ch) {
    case 0: return (x * 7 + y * 3) & 0xff;
    case 1: return (x * 5 + y * 11) & 0xff;
    default: return (x * 13 + y * 17 + 41) & 0xff;
    }
}

static int cap_test_setenv(const char *name, const char *value) {
#ifdef _WIN32
    return SetEnvironmentVariableA(name, value) ? 1 : 0;
#else
    return setenv(name, value, 1) == 0;
#endif
}

/* Byte-exact P6 verification: exact header, exact payload, no trailing bytes. */
static int cap_test_verify_ppm(const char *path, int w, int h) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char hdr[64];
    int ok = 0;
    {
        char expect[64];
        snprintf(expect, sizeof expect, "P6\n%d %d\n255\n", w, h);
        size_t hl = strlen(expect);
        size_t got = fread(hdr, 1, hl, f);
        hdr[got] = '\0';
        if (got != hl || memcmp(hdr, expect, hl) != 0) {
            fprintf(stderr, "verify: header mismatch (got %zu bytes)\n", got);
            for (size_t i = 0; i < got; i++)
                fprintf(stderr, "  hdr byte %02u: %02x\n", (unsigned)i, (unsigned char)hdr[i]);
            return 0;
        }
            size_t body = (size_t)w * (size_t)h * 3u;
            uint8_t *buf = (uint8_t *)malloc(body);
            if (buf) {
                size_t got = fread(buf, 1, body, f);
                int extra = fgetc(f);
                if (got == body && extra == EOF) {
                    ok = 1;
                    for (int y = 0; y < h && ok; y++)
                        for (int x = 0; x < w && ok; x++) {
                            const uint8_t *px = buf + ((size_t)y * (size_t)w + x) * 3u;
                            if (px[0] != (uint8_t)cap_test_pattern(x, y, 0) ||
                                px[1] != (uint8_t)cap_test_pattern(x, y, 1) ||
                                px[2] != (uint8_t)cap_test_pattern(x, y, 2)) {
                                fprintf(stderr, "verify: first mismatch at (%d,%d) got "
                                        "%02x%02x%02x want %02x%02x%02x\n",
                                        x, y, px[0], px[1], px[2],
                                        cap_test_pattern(x, y, 0), cap_test_pattern(x, y, 1),
                                        cap_test_pattern(x, y, 2));
                                ok = 0;
                            }
                        }
                } else {
                    fprintf(stderr, "verify: body got %zu want %zu, extra=%d\n",
                            got, body, extra);
                }
                free(buf);
            }
    }
    fclose(f);
    return ok;
}

static void cap_test_fill_cpu(uint32_t *px, int w, int h) {
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[(size_t)y * (size_t)w + x] = 0xFF000000u |
                ((uint32_t)cap_test_pattern(x, y, 0) << 16) |
                ((uint32_t)cap_test_pattern(x, y, 1) << 8) |
                (uint32_t)cap_test_pattern(x, y, 2);
}

typedef struct CapTestImage {
    VkImage img;
    VkDeviceMemory mem;
    VkBuffer staging;
    VkDeviceMemory staging_mem;
    void *staging_map;
    int w, h;
} CapTestImage;

static void cap_test_image_destroy(CapTestImage *t) {
    if (t->staging_map) vkUnmapMemory(s_dev, t->staging_mem);
    if (t->staging)     vkDestroyBuffer(s_dev, t->staging, NULL);
    if (t->staging_mem) vkFreeMemory(s_dev, t->staging_mem, NULL);
    if (t->img)         vkDestroyImage(s_dev, t->img, NULL);
    if (t->mem)         vkFreeMemory(s_dev, t->mem, NULL);
    memset(t, 0, sizeof *t);
}

/* An R8G8B8A8 image in the exact shape ge_gpu.c presents: TRANSFER_DST|TRANSFER_SRC. */
static int cap_test_image_create(CapTestImage *t, int w, int h) {
    memset(t, 0, sizeof *t);
    t->w = w; t->h = h;
    VkImageCreateInfo imi = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    imi.imageType = VK_IMAGE_TYPE_2D;
    imi.format = VK_FORMAT_R8G8B8A8_UNORM;
    imi.extent.width = (uint32_t)w; imi.extent.height = (uint32_t)h; imi.extent.depth = 1;
    imi.mipLevels = 1; imi.arrayLayers = 1; imi.samples = VK_SAMPLE_COUNT_1_BIT;
    imi.tiling = VK_IMAGE_TILING_OPTIMAL;
    imi.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    imi.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(s_dev, &imi, NULL, &t->img) != VK_SUCCESS) return 0;
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(s_dev, t->img, &mr);
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(s_dev, &mai, NULL, &t->mem) != VK_SUCCESS ||
        vkBindImageMemory(s_dev, t->img, t->mem, 0) != VK_SUCCESS) {
        cap_test_image_destroy(t);
        return 0;
    }
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    bci.size = (VkDeviceSize)((size_t)w * (size_t)h * 4u);
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(s_dev, &bci, NULL, &t->staging) != VK_SUCCESS) {
        cap_test_image_destroy(t);
        return 0;
    }
    vkGetBufferMemoryRequirements(s_dev, t->staging, &mr);
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX)
        mai.memoryTypeIndex = find_mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mai.memoryTypeIndex == UINT32_MAX ||
        vkAllocateMemory(s_dev, &mai, NULL, &t->staging_mem) != VK_SUCCESS ||
        vkBindBufferMemory(s_dev, t->staging, t->staging_mem, 0) != VK_SUCCESS ||
        vkMapMemory(s_dev, t->staging_mem, 0, VK_WHOLE_SIZE, 0, &t->staging_map) != VK_SUCCESS) {
        cap_test_image_destroy(t);
        return 0;
    }
    return 1;
}

/* Upload the pattern and leave the image TRANSFER_SRC_OPTIMAL, exactly the contract
 * sdl3vk_present_image(_ex) requires of GE targets. */
static int cap_test_image_upload(CapTestImage *t) {
    uint8_t *dst = (uint8_t *)t->staging_map;
    for (int y = 0; y < t->h; y++)
        for (int x = 0; x < t->w; x++) {
            uint8_t *px = dst + ((size_t)y * (size_t)t->w + x) * 4u;
            px[0] = (uint8_t)cap_test_pattern(x, y, 0);
            px[1] = (uint8_t)cap_test_pattern(x, y, 1);
            px[2] = (uint8_t)cap_test_pattern(x, y, 2);
            px[3] = 0xFF;
        }
    vkResetCommandBuffer(s_cmd, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(s_cmd, &bi) != VK_SUCCESS) return 0;
    barrier(s_cmd, t->img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bic = {0};
    bic.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    bic.imageSubresource.layerCount = 1;
    bic.imageExtent.width = (uint32_t)t->w; bic.imageExtent.height = (uint32_t)t->h;
    bic.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, t->staging, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    barrier(s_cmd, t->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (vkEndCommandBuffer(s_cmd) != VK_SUCCESS) return 0;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cmd;
    if (vkQueueSubmit(s_queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) return 0;
    if (vkQueueWaitIdle(s_queue) != VK_SUCCESS) return 0;
    return 1;
}

int sdl3vk_capture_selftest(void) {
    int ok = 1;

    /* ---- pure framebuffer-capture policy (fbcap_policy.h); no Vulkan needed --------- */
    if (sr_fbcap_owner(0, 0) != SR_FBCAP_NONE) {
        fprintf(stderr, "policy: nothing due must not own the slot\n"); ok = 0;
    }
    if (sr_fbcap_owner(1, 1) != SR_FBCAP_FBDUMP ||
        sr_fbcap_owner(1, 0) != SR_FBCAP_FBDUMP) {
        fprintf(stderr, "policy: SR_FBDUMP must outrank SR_FBSNAP\n"); ok = 0;
    }
    if (sr_fbcap_owner(0, 1) != SR_FBCAP_FBSNAP) {
        fprintf(stderr, "policy: SR_FBSNAP due must own the slot\n"); ok = 0;
    }
    {
        char p[128];
        if (!sr_fbcap_path(SR_FBCAP_FBSNAP, 12, p, sizeof p) ||
            strcmp(p, "build/snapshots/frame_0012.ppm") != 0) {
            fprintf(stderr, "policy: FBSNAP path wrong: '%s'\n", p); ok = 0;
        }
        if (!sr_fbcap_path(SR_FBCAP_FBDUMP, 0, p, sizeof p) ||
            strcmp(p, "present_source.ppm") != 0) {
            fprintf(stderr, "policy: FBDUMP path wrong: '%s'\n", p); ok = 0;
        }
        if (sr_fbcap_path(SR_FBCAP_NONE, 0, p, sizeof p) || p[0] != '\0') {
            fprintf(stderr, "policy: NONE owner must not produce a path\n"); ok = 0;
        }
    }
    if (sr_fbcap_exit_status(SR_FBCAP_NONE, 1) != 0 ||
        sr_fbcap_exit_status(SR_FBCAP_NONE, -1) != 0 ||
        sr_fbcap_exit_status(SR_FBCAP_FBDUMP, 1) != 0 ||
        sr_fbcap_exit_status(SR_FBCAP_FBDUMP, 0) != 1 ||
        sr_fbcap_exit_status(SR_FBCAP_FBDUMP, -1) != 1) {
        fprintf(stderr, "policy: exit-status table wrong\n"); ok = 0;
    }
    if (!ok) return 1;

    /* ---- validation-layer requirement ---------------------------------------------- */
    {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, NULL);
        VkLayerProperties lp[64];
        int have = 0;
        if (n > 64) n = 64;
        if (vkEnumerateInstanceLayerProperties(&n, lp) == VK_SUCCESS)
            for (uint32_t i = 0; i < n; i++)
                if (strcmp(lp[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) have = 1;
        if (!have) {
            fprintf(stderr, "gpu capture selftest: SKIP (VK_LAYER_KHRONOS_validation "
                            "not installed)\n");
            return 77;
        }
    }
    cap_test_setenv("SR_VULKAN_VALIDATION", "1");
    if (!sdl3vk_init("Nakagawa GPU capture selftest")) {
        fprintf(stderr, "gpu capture selftest: SKIP (Vulkan initialization unavailable)\n");
        return 77;
    }

    /* ---- CPU framebuffer path (B8G8R8A8 fbimg) -------------------------------------- */
    static uint32_t px[PSP_W * PSP_H];
    cap_test_fill_cpu(px, PSP_W, PSP_H);
    if (!sdl3vk_capture_arm("selftest_cpu.ppm")) {
        fprintf(stderr, "cpu: capture arm refused\n"); ok = 0;
    }
    if (sdl3vk_present_rgba(px) != 1) {
        fprintf(stderr, "cpu: present failed\n"); ok = 0;
    }
    if (sdl3vk_capture_result() != 1) {
        fprintf(stderr, "cpu: capture result %d (expected 1)\n", sdl3vk_capture_result());
        ok = 0;
    }
    if (strcmp(sdl3vk_capture_source_label(), "cpu-framebuffer") != 0) {
        fprintf(stderr, "cpu: source label '%s'\n", sdl3vk_capture_source_label());
        ok = 0;
    }
    if (!cap_test_verify_ppm("selftest_cpu.ppm", PSP_W, PSP_H)) {
        fprintf(stderr, "cpu: PPM bytes wrong (header, channel order, pitch, or truncation)\n");
        ok = 0;
    }
    remove("selftest_cpu.ppm");

    /* ---- cancel: an armed capture never serviced reports "nothing attempted" --------- */
    if (!sdl3vk_capture_arm("selftest_cancel.ppm")) {
        fprintf(stderr, "cancel: arm refused\n"); ok = 0;
    }
    sdl3vk_capture_cancel();
    if (sdl3vk_capture_result() != 0) {
        fprintf(stderr, "cancel: result %d (expected 0)\n", sdl3vk_capture_result());
        ok = 0;
    }
    {
        FILE *f = fopen("selftest_cancel.ppm", "rb");
        if (f) { fclose(f); fprintf(stderr, "cancel: file must not exist\n"); ok = 0; }
    }

    /* ---- GPU render-target path (R8G8B8A8, same 480x272 region) --------------------- */
    CapTestImage gpu;
    if (!cap_test_image_create(&gpu, PSP_W, PSP_H) || !cap_test_image_upload(&gpu)) {
        fprintf(stderr, "gpu: image setup failed\n"); ok = 0;
    } else {
        if (!sdl3vk_capture_arm("selftest_gpu.ppm")) {
            fprintf(stderr, "gpu: capture arm refused\n"); ok = 0;
        }
        if (sdl3vk_present_image_ex((void *)gpu.img, PSP_W, PSP_H) != 1) {
            fprintf(stderr, "gpu: present failed\n"); ok = 0;
        }
        if (sdl3vk_capture_result() != 1) {
            fprintf(stderr, "gpu: capture result %d (expected 1)\n", sdl3vk_capture_result());
            ok = 0;
        }
        if (strcmp(sdl3vk_capture_source_label(), "gpu-render-target") != 0) {
            fprintf(stderr, "gpu: source label '%s'\n", sdl3vk_capture_source_label());
            ok = 0;
        }
        if (!cap_test_verify_ppm("selftest_gpu.ppm", PSP_W, PSP_H)) {
            fprintf(stderr, "gpu: PPM bytes wrong (BGRA/RGBA handling, pitch, or truncation)\n");
            ok = 0;
        }
        remove("selftest_gpu.ppm");
        cap_test_image_destroy(&gpu);
    }

    /* ---- scaled GPU path: a larger source exercises row pitch and buffer growth ------ */
    CapTestImage big;
    if (!cap_test_image_create(&big, 960, 544) || !cap_test_image_upload(&big)) {
        fprintf(stderr, "scaled: image setup failed\n"); ok = 0;
    } else {
        if (!sdl3vk_capture_arm("selftest_scaled.ppm")) {
            fprintf(stderr, "scaled: capture arm refused\n"); ok = 0;
        }
        if (sdl3vk_present_image_ex((void *)big.img, 960, 544) != 1) {
            fprintf(stderr, "scaled: present failed\n"); ok = 0;
        }
        if (sdl3vk_capture_result() != 1) {
            fprintf(stderr, "scaled: capture result %d (expected 1)\n", sdl3vk_capture_result());
            ok = 0;
        }
        if (!cap_test_verify_ppm("selftest_scaled.ppm", 960, 544)) {
            fprintf(stderr, "scaled: PPM bytes wrong (pitch or truncation)\n"); ok = 0;
        }
        remove("selftest_scaled.ppm");
        cap_test_image_destroy(&big);
    }

    /* ---- present error recovery and disposition tests ------------------------------- */
    {
        /* Test 1: VK_SUBOPTIMAL_KHR (enqueued rebuild) */
        uint64_t sc0 = sdl3vk_swapchain_generation();
        sdl3vk_present_fault_inject(VK_SUBOPTIMAL_KHR);
        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "present fault SUBOPTIMAL: expected present success (1)\n"); ok = 0;
        }
        if (sdl3vk_swapchain_generation() <= sc0) {
            fprintf(stderr, "present fault SUBOPTIMAL: expected swapchain rebuild\n"); ok = 0;
        }

        /* Test 2: VK_ERROR_OUT_OF_DATE_KHR (enqueued present / stale swapchain) */
        sc0 = sdl3vk_swapchain_generation();
        uint64_t sem0 = sdl3vk_frame_semaphore_generation();
        if (!sdl3vk_capture_arm("selftest_ood.ppm")) {
            fprintf(stderr, "present fault OOD: arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_DATE_KHR);
        if (sdl3vk_present_rgba(px) != -1) {
            fprintf(stderr, "present fault OOD: expected present failure (-1)\n"); ok = 0;
        }
        if (sdl3vk_capture_result() != -1) {
            fprintf(stderr, "present fault OOD: capture result must fail (-1)\n"); ok = 0;
        }
        if (sdl3vk_swapchain_generation() <= sc0) {
            fprintf(stderr, "present fault OOD: expected swapchain rebuild\n"); ok = 0;
        }
        if (sdl3vk_frame_semaphore_generation() != sem0) {
            fprintf(stderr, "present fault OOD: enqueued present must NOT recreate semaphore\n"); ok = 0;
        }

        /* Test 3: VK_ERROR_OUT_OF_HOST_MEMORY (unenqueued hard error / semaphore recovery) */
        sem0 = sdl3vk_frame_semaphore_generation();
        if (!sdl3vk_capture_arm("selftest_oom.ppm")) {
            fprintf(stderr, "present fault OOM: arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject(VK_ERROR_OUT_OF_HOST_MEMORY);
        if (sdl3vk_present_rgba(px) != -1) {
            fprintf(stderr, "present fault OOM: expected present failure (-1)\n"); ok = 0;
        }
        if (sdl3vk_capture_result() != -1) {
            fprintf(stderr, "present fault OOM: capture result must fail (-1)\n"); ok = 0;
        }
        if (sdl3vk_frame_semaphore_generation() <= sem0) {
            fprintf(stderr, "present fault OOM: expected frame semaphore recovery/recreation\n"); ok = 0;
        }

        /* Verify frame slot recovery: next normal present must succeed cleanly without validation errors */
        if (sdl3vk_present_rgba(px) != 1) {
            fprintf(stderr, "present recovery: post-OOM normal present failed\n"); ok = 0;
        }

        /* Test 4: VK_ERROR_SURFACE_LOST_KHR (enqueued present / surface lost terminal state) */
        sem0 = sdl3vk_frame_semaphore_generation();
        if (!sdl3vk_capture_arm("selftest_surflost.ppm")) {
            fprintf(stderr, "present fault SURFLOST: arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject(VK_ERROR_SURFACE_LOST_KHR);
        if (sdl3vk_present_rgba(px) != -1) {
            fprintf(stderr, "present fault SURFLOST: expected present failure (-1)\n"); ok = 0;
        }
        if (sdl3vk_capture_result() != -1) {
            fprintf(stderr, "present fault SURFLOST: capture result must fail (-1)\n"); ok = 0;
        }
        if (!sdl3vk_renderer_terminal()) {
            fprintf(stderr, "present fault SURFLOST: expected renderer terminal state\n"); ok = 0;
        }
        if (sdl3vk_frame_semaphore_generation() != sem0) {
            fprintf(stderr, "present fault SURFLOST: enqueued present must NOT recreate semaphore\n"); ok = 0;
        }

        /* Test 5: VK_ERROR_DEVICE_LOST (terminal error, no normal recovery) */
        sem0 = sdl3vk_frame_semaphore_generation();
        /* Reset terminal state and recreate swapchain so next acquire succeeds */
        s_renderer_terminal = 0;
        create_swapchain();
        if (!sdl3vk_capture_arm("selftest_devlost.ppm")) {
            fprintf(stderr, "present fault DEVLOST: arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject(VK_ERROR_DEVICE_LOST);
        if (sdl3vk_present_rgba(px) != -1) {
            fprintf(stderr, "present fault DEVLOST: expected present failure (-1)\n"); ok = 0;
        }
        if (!sdl3vk_renderer_terminal()) {
            fprintf(stderr, "present fault DEVLOST: expected renderer terminal state\n"); ok = 0;
        }
        if (sdl3vk_frame_semaphore_generation() != sem0) {
            fprintf(stderr, "present fault DEVLOST: lost device must NOT recreate semaphore\n"); ok = 0;
        }

        /* Test 6: VK_ERROR_UNKNOWN / unclassified error (unknown enqueue state -> terminal fail-closed) */
        sem0 = sdl3vk_frame_semaphore_generation();
        s_renderer_terminal = 0;
        create_swapchain();
        if (!sdl3vk_capture_arm("selftest_unknown.ppm")) {
            fprintf(stderr, "present fault UNKNOWN: arm failed\n"); ok = 0;
        }
        sdl3vk_present_fault_inject((VkResult)-9999);
        if (sdl3vk_present_rgba(px) != -1) {
            fprintf(stderr, "present fault UNKNOWN: expected present failure (-1)\n"); ok = 0;
        }
        if (!sdl3vk_renderer_terminal()) {
            fprintf(stderr, "present fault UNKNOWN: expected renderer terminal state\n"); ok = 0;
        }
        if (sdl3vk_frame_semaphore_generation() != sem0) {
            fprintf(stderr, "present fault UNKNOWN: unclassified error must NOT recreate semaphore\n"); ok = 0;
        }
        if (sdl3vk_capture_arm("selftest_terminal_refused.ppm")) {
            fprintf(stderr, "terminal state: capture arm must be refused when terminal\n"); ok = 0;
        }
    }

    int errors = sdl3vk_validation_error_count();
    if (errors != 0) {
        fprintf(stderr, "validation: %d ERROR-severity messages; expected 0\n", errors);
        ok = 0;
    }
    sdl3vk_shutdown();
    if (!ok) return 1;
    puts("gpu capture selftest: OK");
    return 0;
}

void sdl3vk_shutdown(void) {
    if (s_dev) {
        (void)wait_all_present_frames();
        /* Presentation-engine ownership is not covered by a submit fence.  A queue-idle
         * at final teardown is therefore still required before destroying the swapchain;
         * unlike vkDeviceWaitIdle this does not stall unrelated device queues. */
        vkQueueWaitIdle(s_queue);
    }
    destroy_swapchain();
    for (int i = 0; i < PRESENT_FRAMES; i++) {
        PresentFrame *f = &s_frame[i];
        if (f->staging_map) vkUnmapMemory(s_dev, f->staging_mem);
        if (f->fbimg)       vkDestroyImage(s_dev, f->fbimg, NULL);
        if (f->fbimg_mem)   vkFreeMemory(s_dev, f->fbimg_mem, NULL);
        if (f->staging)     vkDestroyBuffer(s_dev, f->staging, NULL);
        if (f->staging_mem) vkFreeMemory(s_dev, f->staging_mem, NULL);
        if (f->sem_acq)     vkDestroySemaphore(s_dev, f->sem_acq, NULL);
        if (f->sem_done)    vkDestroySemaphore(s_dev, f->sem_done, NULL);
        if (f->fence)       vkDestroyFence(s_dev, f->fence, NULL);
    }
    if (s_fence)       vkDestroyFence(s_dev, s_fence, NULL);
    cap_free_buffer();
    s_cap_state = CAP_IDLE;
    s_cap_path[0] = '\0';
    s_cap_result = 0;
    if (s_pool)        vkDestroyCommandPool(s_dev, s_pool, NULL);
    if (s_dev)         vkDestroyDevice(s_dev, NULL);
    if (s_surf)        vkDestroySurfaceKHR(s_inst, s_surf, NULL);
    if (s_inst)        vkDestroyInstance(s_inst, NULL);
    if (s_pad)         SDL_CloseGamepad(s_pad);
    if (s_win)         SDL_DestroyWindow(s_win);
    SDL_Quit();
}
