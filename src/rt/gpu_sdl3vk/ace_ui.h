/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_ui.h - Ace Mode's in-game UI (src/rt/ace.h): the launch notice, the speed readout and the
 * crisp on-screen keyboard, drawn at the window's resolution over the frame the presenter
 * blitted onto the swapchain image, through nk_ui's Vulkan backend (src/ui/backend/nk_ui_vk.c).
 *
 * Its pixels exist only on the swapchain image: no guest-visible byte, guest frame or capture
 * changes. Only an Ace run makes one (the presenter's UI pass is registered only then), so an
 * authentic run draws exactly what it drew before.
 *
 * It loads every Vulkan function through the instance's vkGetInstanceProcAddr and calls nothing
 * of the runtime: ace-ui-selftest links it with nk_ui, its Vulkan backend and the C library
 * alone (invariant S1).
 */
#ifndef SR_ACE_UI_H
#define SR_ACE_UI_H

#include "nk_ui.h"
#include "../osk_overlay.h"

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct AceUi AceUi;

typedef struct {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
    uint32_t frames_in_flight;           /* the presenter's frame slots */
    const NkUiTextBackend *text;         /* the UI fonts; must outlive the AceUi */
} AceUiInit;

/* What one present shows. NULL or empty strings show nothing. */
typedef struct {
    int pixel_w, pixel_h;                /* the swapchain image */
    float display_scale;                 /* the window's display scale (1.0 at 100%) */
    double now_s;
    int reduce_motion;                   /* the player's choice: the notice cuts */
    int high_contrast;                   /* paper on surface_high with a strong edge */
    const char *notice;                  /* the active features in words, or NULL */
    float notice_alpha;                  /* 0 to 1 */
    const char *readout;                 /* "100%  30 FPS", or NULL */
    int readout_warning;                 /* PC LIMITED: in warning */
    const OskOverlay *keyboard;          /* an open keyboard, or NULL */
} AceUiFrame;

/* The swapchain image a present draws into, in TRANSFER_DST_OPTIMAL before and after. */
typedef struct {
    VkCommandBuffer cmd;
    uint32_t frame_slot;                 /* [0, frames_in_flight), its fence already waited */
    VkFormat format;
    VkExtent2D extent;
    const VkImage *images;
    uint32_t image_count;
    uint32_t image_index;
    uint64_t swapchain_gen;              /* changes when the images do */
} AceUiTarget;

/* NULL, with the reason, when a Vulkan object cannot be made or the backend is not built in. */
AceUi *ace_ui_create(const AceUiInit *init, char *error, size_t error_size);
/* Waits for the queue to go idle, then frees everything. NULL is fine. */
void ace_ui_destroy(AceUi *ui);
/* Whether the frame has anything to draw. */
int ace_ui_frame_wants(const AceUiFrame *frame);
/* Records the frame into target->cmd: the backend's uploads, then one render pass that loads
 * the image and draws over it. Returns 1 when it drew, 0 when there was nothing to draw or it
 * could not (then nothing that draws was recorded). */
int ace_ui_record(AceUi *ui, const AceUiTarget *target, const AceUiFrame *frame);

/* The frame's viewport: its window at its display scale, with a smallest layout of
 * ACE_UI_MIN_LOGICAL_W x ACE_UI_MIN_LOGICAL_H, so in a small window (a 480 x 272 game window)
 * the keyboard is drawn smaller rather than cut off. */
#define ACE_UI_MIN_LOGICAL_W 640.0f
#define ACE_UI_MIN_LOGICAL_H 420.0f
void ace_ui_viewport(const AceUiFrame *frame, NkUiViewport *out);
/* CPU only, for tests and for ace_ui_record: the frame's layout in an open nk_ui frame whose
 * viewport is ace_ui_viewport's. */
void ace_ui_layout(NkUi *ui, const AceUiFrame *frame);

/* Element ids, in the overlay's family (nk_overlay.h) above its pages' range. */
#define ACE_UI_ID(offset) NK_UI_ID(NK_UI_ID_SCREEN_OVERLAY, 0x00A000u + (offset))
#define ACE_UI_ID_ROOT ACE_UI_ID(0x001u)
#define ACE_UI_ID_TOP ACE_UI_ID(0x002u)
#define ACE_UI_ID_READOUT ACE_UI_ID(0x003u)
#define ACE_UI_ID_READOUT_TEXT ACE_UI_ID(0x004u)
#define ACE_UI_ID_SPACER ACE_UI_ID(0x005u)
#define ACE_UI_ID_NOTICE ACE_UI_ID(0x006u)
#define ACE_UI_ID_NOTICE_HEAD ACE_UI_ID(0x007u)
#define ACE_UI_ID_NOTICE_TEXT ACE_UI_ID(0x008u)
#define ACE_UI_ID_FILL ACE_UI_ID(0x009u)
#define ACE_UI_ID_KEYBOARD ACE_UI_ID(0x010u)
#define ACE_UI_ID_KEYBOARD_DESC ACE_UI_ID(0x011u)
#define ACE_UI_ID_KEYBOARD_FIELD ACE_UI_ID(0x012u)
#define ACE_UI_ID_KEYBOARD_TEXT ACE_UI_ID(0x013u)
#define ACE_UI_ID_KEYBOARD_HINT ACE_UI_ID(0x014u)
#define ACE_UI_ID_KEYBOARD_GRID ACE_UI_ID(0x015u)
#define ACE_UI_ID_KEYBOARD_ROWS ACE_UI_ID(0x020u)  /* + row */
#define ACE_UI_ID_KEYBOARD_KEYS ACE_UI_ID(0x100u)  /* + 2 * cell, + 1 its label */

#endif /* SR_ACE_UI_H */
