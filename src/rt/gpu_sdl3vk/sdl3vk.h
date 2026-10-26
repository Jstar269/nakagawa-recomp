// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.
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
#include <stdbool.h>
#include "nk_input_profile.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Create the SDL3 window and the Vulkan device/swapchain. Returns 1 on success, 0 on any
 * failure (caller falls back to the GDI path). */
int  sdl3vk_init(const char *title);

/* Raise the initialized SDL window. On Windows, also request OS foreground activation. */
bool sdl3vk_raise_window(void);

/* Swapchain present-mode choice for the SR_VSYNC setting (pure, no window needed).
 * `available` lists VkPresentModeKHR values as plain ints so this header stays
 * Vulkan-free: 0 = IMMEDIATE, 1 = MAILBOX, 2 = FIFO. vsync != 0 locks presentation
 * to the display refresh (FIFO); vsync == 0 is uncapped, preferring MAILBOX, then
 * IMMEDIATE, then FIFO as the guaranteed fallback. available may be NULL when
 * count == 0. */
int sdl3vk_pick_present_mode(int vsync, const int *available, int count);

/* Surface-format choice for the swapchain (pure, no device needed), Vulkan-free like the
 * present-mode picker. `query` is the VkResult of vkGetPhysicalDeviceSurfaceFormatsKHR as an
 * int (0 = VK_SUCCESS, 5 = VK_INCOMPLETE). VK_INCOMPLETE means the surface has more formats
 * than the caller's array holds; the entries written are valid, so it is accepted. `formats`
 * lists the VkFormat values as plain ints; only the first min(count, SDL3VK_SURFACE_FORMAT_CAP)
 * entries are read. Returns the index of the first VK_FORMAT_B8G8R8A8_UNORM (44) entry, else 0.
 * Returns -1 when the query failed, count is zero, or formats is NULL. */
#define SDL3VK_SURFACE_FORMAT_CAP 32
int sdl3vk_pick_surface_format(int query, int count, const int *formats);

/* How a game frame is scaled into the window (issue #1129), chosen by SR_PRESENT_SCALING:
 *   sharp    (default) fit the window keeping the 480:272 aspect; at a fractional scale the frame
 *            is first enlarged with nearest filtering to the next integer multiple (the smaller
 *            one when the axes round apart), then that image is filtered linearly to the fitted
 *            size (sharp bilinear);
 *   integer  the largest integer multiple of the source that fits, letterboxed, nearest filtering
 *            (a window smaller than the source falls back to a linear fit);
 *   bilinear fit the window with linear filtering (the behaviour before issue #1129).
 * Every mode uses one nearest blit at an exact integer fit, so a 1x, 2x or 3x window is unchanged. */
enum {
    SDL3VK_SCALING_SHARP = 0,
    SDL3VK_SCALING_INTEGER = 1,
    SDL3VK_SCALING_BILINEAR = 2,
};

/* The mode for an SR_PRESENT_SCALING value: NULL or empty is SDL3VK_SCALING_SHARP, an unknown
 * value -1. */
int sdl3vk_parse_present_scaling(const char *value);

typedef struct {
    int x, y, w, h;  /* destination rectangle in the swapchain image; w == 0 means draw nothing */
    int prescale;    /* > 1: nearest blit into a (prescale*srcw x prescale*srch) image first */
    int nearest;     /* filter of the blit that writes the destination: 1 nearest, 0 linear */
} Sdl3VkPresentPlan;

/* Destination and filtering for a srcw x srch frame in a dw x dh swapchain image (pure, no
 * device needed). max_dim bounds the prescale image (maxImageDimension2D); a sharp prescale
 * beyond it falls back to one linear blit. */
Sdl3VkPresentPlan sdl3vk_present_plan(int mode, int srcw, int srch, int dw, int dh, int max_dim);

/* In-game performance HUD control (F3 / SR_HUD=1) */
int  sdl3vk_hud_is_enabled(void);
void sdl3vk_hud_set_enabled(int enabled);
void sdl3vk_set_audio_active_cb(int (*cb)(void));

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

/* Visual-evidence capture (issue #57) is presenter-neutral: it is armed, published and
 * reported through fbcap.h, which owns all capture state. When a capture is armed, this
 * presenter records a readback of its presentation source (the CPU framebuffer image or the
 * GE render target) inside the same command buffer that presents the frame -- no extra
 * swapchain acquire, no content-destroying layout transition -- and hands the readback to
 * fbcap.c only after the frame is known to have reached the presentation engine. A present
 * that fails resolves the armed capture as failed. */

/* 1 once the renderer has latched a terminal state: the device was lost, or a presentation
 * recovery sequence itself failed. Presentation is refused from then on, and a capture
 * armed for a refused present resolves as failed. */
int  sdl3vk_renderer_terminal(void);

/* Test-only present fault injection seam for selftests. Takes an int so this header
 * stays vulkan.h-free (pass VkResult value). */
void     sdl3vk_present_fault_inject(int vk_result);
void     sdl3vk_present_fault_clear(void);
uint64_t sdl3vk_swapchain_generation(void);
uint64_t sdl3vk_frame_semaphore_generation(void);

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

/* Text entry for the in-window on-screen keyboard (src/rt/osk_overlay.c). While claimed, the
 * window accepts typed text, Escape cancels instead of quitting, and the editing keys are
 * queued here instead of reaching the guest. Keys are neutral codes, not SDL keycodes. */
typedef enum {
    SDL3VK_KEY_NONE = 0,
    SDL3VK_KEY_UP,
    SDL3VK_KEY_DOWN,
    SDL3VK_KEY_LEFT,
    SDL3VK_KEY_RIGHT,
    SDL3VK_KEY_ENTER,
    SDL3VK_KEY_ESCAPE,
    SDL3VK_KEY_TAB,
    SDL3VK_KEY_BACKSPACE,
    SDL3VK_KEY_TEXT        /* codepoint holds one Unicode character typed */
} Sdl3VkKey;

typedef struct {
    int      key;          /* an Sdl3VkKey */
    uint32_t codepoint;    /* SDL3VK_KEY_TEXT only */
} Sdl3VkKeyEvent;

void sdl3vk_set_text_claim(int on);
/* Drains up to max queued key and text events in order. Returns the count. */
int  sdl3vk_take_key_events(Sdl3VkKeyEvent *out, int max);
/* PSP button bits of gamepad buttons that went down since the last take. The keyboard's pad
 * input: a press between two samples is not lost the way a held state would be. */
uint32_t sdl3vk_take_pad_pulses(void);

/* ---- in-game pause (#1159) ----------------------------------------------------------
 * Esc, F1 and the pad's Guide button ask for the pause overlay. They are consumed: none of
 * them reaches the game, and Esc no longer closes the window. Losing the open pad asks for
 * a pause too. The request is latched until taken; it is not latched while the input is
 * claimed (the overlay is already open). */
enum {
    SDL3VK_PAUSE_REQUEST_NONE = 0,
    SDL3VK_PAUSE_REQUEST_USER = 1,      /* Esc, F1 or Guide */
    SDL3VK_PAUSE_REQUEST_PAD_LOST = 2   /* the open gamepad was disconnected */
};
int sdl3vk_take_pause_request(void);

/* Input claim. While a sink is set every input and window event goes to it (sdl_event is a
 * const SDL_Event *), and the game reads every button released with the stick centred.
 * Clearing the sink ends the claim: a button or stick still held then stays hidden from
 * the game until it is let go (nk_input_gate_*), so the press that closed the overlay never
 * reaches the game. Presses the game had not sampled when the claim began are kept for it,
 * except for a button still held at the end. */
typedef void (*Sdl3VkInputSink)(const void *sdl_event, void *user);
void sdl3vk_set_input_claim(Sdl3VkInputSink sink, void *user);
int  sdl3vk_input_claimed(void);

/* The pause presenter draws the window itself while the game is stopped: one image at the
 * swapchain's pixel size, composed by the caller. sdl3vk_overlay_extent gives that size and
 * its byte order (rgba 1: R,G,B,A in memory; 0: B,G,R,A); it returns 0 when no image can be
 * shown now (minimised, no swapchain, or a surface format with another layout). */
int  sdl3vk_overlay_extent(int *w, int *h, int *rgba);
/* Shows the overlay image. px (w*h pixels of `pitch` bytes per row, at the current extent)
 * replaces the uploaded image; NULL shows the last one again. These presents are not game
 * frames: no frame capture, perf count or HUD. Returns 1 when presented, 2 when the extent
 * changed and the image must be composed again, 0 when the window was closed and -1 on a
 * presentation failure. */
int  sdl3vk_present_overlay(const void *px, int pitch);
/* Frees the overlay image when the pause ends. */
void sdl3vk_overlay_release(void);
int  sdl3vk_window_minimized(void);
/* Waits until an event is queued or timeout_ms passes, without taking the event. */
void sdl3vk_wait_events(uint32_t timeout_ms);
/* The SR_PRESENT_SCALING mode in use (SDL3VK_SCALING_*). */
int  sdl3vk_present_scaling(void);
/* The refresh period of the display the window is on, in ns; 0 when SDL does not know it. */
uint64_t sdl3vk_display_period_ns(void);

/* Host input profile mapping and evaluation (#357). */
void sdl3vk_map_gamepad(
    const NkInputProfile *profile,
    const bool host_buttons[NK_HOST_BUTTON_COUNT],
    const int16_t host_axes[NK_HOST_AXIS_COUNT],
    uint32_t *out_buttons,
    uint8_t *out_lx,
    uint8_t *out_ly
);
/* Host keyboard (#1299): PSP button bits for the held keys (`key_state` indexed by
 * SDL_Scancode, `key_count` entries) and for one key-down event. A NULL profile means
 * the default mapping. */
uint32_t sdl3vk_map_keyboard(const NkInputProfile *profile, const bool *key_state, int key_count);
uint32_t sdl3vk_map_key(const NkInputProfile *profile, int scancode);
void sdl3vk_init_input_profile(void);
void sdl3vk_reset_input_profile(void);
const NkInputProfile *sdl3vk_get_input_profile(void);
int  sdl3vk_input_profile_selftest(void);
/* The pause keys, the overlay's input claim and the release mask, driven through the real
 * event path with a virtual gamepad (gpu-capture-selftest). Returns 1 on success. */
int  sdl3vk_pause_input_selftest(void);

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
