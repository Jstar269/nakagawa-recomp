/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_smooth.h - Ace Mode's in-between frames (src/rt/ace.h, docs/ENHANCEMENT_CONTRACT.md section
 * 10): block-matched motion over two private copies of the presented frames, recorded into the
 * present's command buffer. Needs a queue with compute and nothing of the GE: it works on what
 * the presenter already holds, GPU target or CPU frame alike.
 *
 * Every image here is private. The in-between is shown by blitting it to the swapchain, so it
 * never reaches guest VRAM, a capture, or anything the game can read.
 *
 * It loads every Vulkan function through the instance's vkGetInstanceProcAddr and calls nothing
 * of the runtime: ace-smooth-selftest links it with the C library alone (invariant S1).
 */
#ifndef SR_ACE_SMOOTH_H
#define SR_ACE_SMOOTH_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "../ace.h"   /* AceSmoothAuxStats */

typedef struct AceBetween AceBetween;

typedef struct {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    VkQueue queue;
    uint32_t queue_family;
} AceSmoothInit;

/* NULL, with the reason, when the queue has no compute or a Vulkan object cannot be made. */
AceBetween *ace_smooth_create(const AceSmoothInit *init, char *error, size_t error_size);
/* Waits for the device to go idle, then frees everything. NULL is fine. */
void ace_smooth_destroy(AceBetween *s);

/* The HUD mask is one bit per guest pixel of the 480x272 display, row-major, bit (i & 31) of word
 * i / 32: set where the frame's through-mode (2D) draws covered it. */
#define ACE_SMOOTH_HUD_W 480
#define ACE_SMOOTH_HUD_H 272
#define ACE_SMOOTH_HUD_WORDS ((ACE_SMOOTH_HUD_W * ACE_SMOOTH_HUD_H) / 32)

/* What the GE knows about a presented frame besides its pixels. Either part may be absent. */
typedef struct {
    /* ACE_SMOOTH_HUD_WORDS words, or NULL: no mask (the frame is matched as before). */
    const uint32_t *hud_bits;
    /* The frame's D16 depth image, in depth_layout, with the presented frame at its top-left at
     * the same scale; VK_NULL_HANDLE when the frame wrote no depth. It is only read, and is left
     * in depth_layout. It needs VK_IMAGE_USAGE_TRANSFER_SRC_BIT. */
    VkImage depth;
    VkImageLayout depth_layout;
    int depth_w, depth_h;
} AceSmoothAux;

/* Copies `src` (w x h, in TRANSFER_SRC_OPTIMAL) into the private frames. With `between`, and a
 * previous frame of the same size, also records the in-between of the two and returns it in
 * TRANSFER_SRC_OPTIMAL; otherwise returns VK_NULL_HANDLE. `src` is only read. A size change
 * waits for the device and starts over. */
VkImage ace_smooth_record(AceBetween *s, VkCommandBuffer cmd, VkImage src, int w, int h, int between);
/* The same, with the frame's HUD mask and depth (`aux` may be NULL, which is ace_smooth_record).
 * Masked pixels never steer the motion search and are never dragged by it; depth makes a vector
 * that lands on one surface in both frames fit better than one that crosses an edge. The mask
 * and depth are used only when both frames of the pair have them. */
VkImage ace_smooth_record_aux(AceBetween *s, VkCommandBuffer cmd, VkImage src, int w, int h, int between,
                              const AceSmoothAux *aux);
/* Records the transition of the newest private frame to TRANSFER_SRC_OPTIMAL and returns it,
 * for the present that shows the held frame. VK_NULL_HANDLE when there is none. */
VkImage ace_smooth_held(AceBetween *s, VkCommandBuffer cmd);
/* The pair counts (pairs, pairs_mask, pairs_depth, depth_refused) of AceSmoothAuxStats; the
 * other fields are left as they are. */
void ace_smooth_aux_stats(const AceBetween *s, AceSmoothAuxStats *out);
/* The size of what ace_smooth_held returns. */
void ace_smooth_size(const AceBetween *s, int *w, int *h);

#endif /* SR_ACE_SMOOTH_H */
