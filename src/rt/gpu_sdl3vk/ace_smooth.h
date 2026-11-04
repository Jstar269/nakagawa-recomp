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

/* Copies `src` (w x h, in TRANSFER_SRC_OPTIMAL) into the private frames. With `between`, and a
 * previous frame of the same size, also records the in-between of the two and returns it in
 * TRANSFER_SRC_OPTIMAL; otherwise returns VK_NULL_HANDLE. `src` is only read. A size change
 * waits for the device and starts over. */
VkImage ace_smooth_record(AceBetween *s, VkCommandBuffer cmd, VkImage src, int w, int h, int between);
/* Records the transition of the newest private frame to TRANSFER_SRC_OPTIMAL and returns it,
 * for the present that shows the held frame. VK_NULL_HANDLE when there is none. */
VkImage ace_smooth_held(AceBetween *s, VkCommandBuffer cmd);
/* The size of what ace_smooth_held returns. */
void ace_smooth_size(const AceBetween *s, int *w, int *h);

#endif /* SR_ACE_SMOOTH_H */
