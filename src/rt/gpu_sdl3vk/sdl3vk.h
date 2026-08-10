// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
/*
 * C ABI consumed by gui.c when built with -DSR_SDL3VK. Phase 0 presents the software-GE
 * framebuffer through a Vulkan swapchain (SDL3 owns the window and all input devices);
 * later phases move GE rasterization itself onto the GPU behind this same boundary.
 *
 * This is the project's own Vulkan backend, written from scratch; it does not reuse
 * PPSSPP's GPU. (An earlier PPSSPP-GPU bridge under src/rt/gpu_vk was a frozen experiment
 * and has been removed.)
 */
#ifndef SR_SDL3VK_H
#define SR_SDL3VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Create the SDL3 window and the Vulkan device/swapchain. Returns 1 on success, 0 on any
 * failure (caller falls back to the GDI path). */
int  sdl3vk_init(const char *title);

/* Pumps the SDL event loop without presenting a frame. Returns 0 on quit, 1 otherwise. */
int  sdl3vk_poll(void);

/* Present one 480x272 frame. px points to 480*272 little-endian BGRA words (the same
 * packing gui.c already produces for StretchDIBits: (r<<16)|(g<<8)|b). Pumps the SDL
 * event loop and samples input. Returns 1 when presented, 0 when the user closed the
 * window / pressed ESC, and -1 for a recoverable presentation failure. */
int  sdl3vk_present_rgba(const uint32_t *px);

/* Present directly from a VkImage owned by the GPU rasterizer (ge_gpu.c). The image
 * must be RGBA8 in TRANSFER_SRC_OPTIMAL layout; the visible region (480x272, or its
 * render-scaled multiple via the _ex form) is blitted with the same letterboxing as
 * sdl3vk_present_rgba. Same return semantics. */
int  sdl3vk_present_image(void *vk_image);
int  sdl3vk_present_image_ex(void *vk_image, int srcw, int srch);

/* Wait only for presentation submissions that still read `vk_image`.  GE target
 * destruction uses this instead of draining the entire Vulkan device. */
int  sdl3vk_wait_image(void *vk_image);

/* Visual-evidence capture (issue #57): arm before the present you want, then read the
 * result after that present returns. The armed capture is recorded inside the same
 * command buffer that presents the frame -- copied from the presentation source while it
 * is still in TRANSFER_SRC_OPTIMAL -- and is published only after the frame is known to
 * have reached the presentation engine. There is no extra swapchain acquire and no
 * content-destroying layout transition, so the file describes exactly what the runtime
 * presented.
 *
 * sdl3vk_capture_arm(path)  arm a capture of the next presented frame. Returns 1 when
 *                           armed, 0 when the renderer is unavailable (no Vulkan device)
 *                           or a capture is already pending.
 * sdl3vk_capture_result()   1 = written, 0 = nothing attempted, -1 = attempted and failed.
 * sdl3vk_capture_cancel()   resolve an armed-but-unserviced capture as "nothing attempted"
 *                           (the present never ran, e.g. the frame slot was skipped).
 * sdl3vk_capture_source_label()  "cpu-framebuffer" or "gpu-render-target" describing the
 *                           presentation source of the most recent capture, or "" when
 *                           no capture has been recorded. Lets diagnostics distinguish
 *                           a capture of the CPU (BGRA) framebuffer from one of the GE
 *                           (RGBA) render target.
 *
 * The written file is a P6 PPM named with a .ppm extension: the format matches the name. */
int  sdl3vk_capture_arm(const char *path);
int  sdl3vk_capture_result(void);
void sdl3vk_capture_cancel(void);
const char *sdl3vk_capture_source_label(void);

/* Validation-layer diagnostics (issue #57): when SR_VULKAN_VALIDATION=1 the renderer
 * enables VK_LAYER_KHRONOS_validation and a debug messenger that prints ERROR/WARNING
 * messages to stderr with a [VulkanValidation] prefix. Init fails closed if the layer was
 * requested but is not installed. sdl3vk_validation_error_count() returns the number of
 * ERROR-severity messages seen since init, which the capture selftest asserts is zero. */
int  sdl3vk_validation_error_count(void);

/* Deterministic present-capture regression (issue #57): arms captures, drives the
 * production present path with synthetic pixels (CPU framebuffer and GPU render-target
 * paths), and byte-checks the published P6 PPMs. Returns 0 = all captures byte-exact and
 * validation-clean, 77 = SKIP (no Vulkan or no validation layer), 1 = failure. */
int  sdl3vk_capture_selftest(void);

/* Input state captured by the last present (PSP sceCtrl button mask / analog stick). */
uint32_t sdl3vk_buttons(void);
void     sdl3vk_consume_button_pulses(void);
void     sdl3vk_analog(uint8_t *lx, uint8_t *ly);
int      sdl3vk_pad_present(void);

/* Vulkan objects shared with the Phase-1 GPU rasterizer (ge_gpu.c). Handles are typed
 * void* here so this header stays vulkan.h-free; they are the real VkInstance etc.
 * Returns 0 until sdl3vk_init() has succeeded. */
typedef struct Sdl3VkInfo {
    void    *instance, *physical, *device, *queue;
    uint32_t queue_family;
} Sdl3VkInfo;
int sdl3vk_get_vk(Sdl3VkInfo *out);

void sdl3vk_shutdown(void);

#ifdef __cplusplus
}
#endif

#endif
