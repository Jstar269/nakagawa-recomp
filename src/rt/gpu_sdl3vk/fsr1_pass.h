/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr1_pass.h - the presentation upscaler (#1273): AMD FidelityFX Super Resolution 1, EASU
 * (edge-adaptive spatial upsampling) then RCAS (robust contrast-adaptive sharpening), from the
 * frame the presenter shows to the size it is shown at. The algorithm is AMD's, in
 * third_party/fidelityfx_fsr1 (MIT); this module records it into the present's command buffer.
 *
 * Presentation only. The pass reads the shown image and writes private images; the window shows
 * the result. The image the guest drew, guest VRAM, a capture of the guest frame and everything a
 * game can read stay as they were. Off unless SR_PRESENT_UPSCALE=fsr1 asks for it.
 *
 * It loads every Vulkan function through the instance's vkGetInstanceProcAddr and calls nothing
 * of the runtime: fsr1-selftest links it with the C library and libm alone.
 */
#ifndef SR_FSR1_PASS_H
#define SR_FSR1_PASS_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

typedef struct Fsr1Pass Fsr1Pass;

typedef struct {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    uint32_t queue_family;
    /* Selftest seam: SPIR-V to use for EASU or RCAS instead of the built-in shader (a mutant).
     * NULL in the player. */
    const uint32_t *easu_spirv;
    size_t easu_spirv_bytes;
    const uint32_t *rcas_spirv;
    size_t rcas_spirv_bytes;
} Fsr1Init;

/* AMD's default sharpness, in stops below the maximum (0 is the sharpest). */
#define FSR1_SHARPNESS_DEFAULT 0.2f
/* A sharpness at or below this skips RCAS: EASU alone. */
#define FSR1_SHARPNESS_OFF (-1.0f)

/* SR_PRESENT_UPSCALE: NULL, empty or "off" is 0, "fsr1" is 1, anything else -1. */
int fsr1_parse_mode(const char *value);

/* NULL, with the reason, when the queue has no compute or a Vulkan object cannot be made. */
Fsr1Pass *fsr1_create(const Fsr1Init *init, char *error, size_t error_size);
/* Waits for the device to go idle, then frees everything. NULL is fine. */
void fsr1_destroy(Fsr1Pass *p);

/* Records the upscale of `src` (w x h, in TRANSFER_SRC_OPTIMAL, any 8-bit-per-channel colour
 * format a blit converts) to out_w x out_h into the command buffer, and returns the private
 * result in TRANSFER_SRC_OPTIMAL. `src` is only read. A sharpness at or below FSR1_SHARPNESS_OFF
 * skips RCAS. A size change waits for the device and rebuilds the images. VK_NULL_HANDLE when the
 * images cannot be made (a size past the device's image limit, or no memory); the caller then
 * shows `src` as it would without the pass. That failure is said once on stderr, and the same
 * sizes are not tried again, so a failed size costs no device wait on later presents. */
VkImage fsr1_record(Fsr1Pass *p, VkCommandBuffer cmd, VkImage src, int w, int h, int out_w,
                    int out_h, float sharpness);

/* How many times the pass has built its images for a new size (a device wait each time). */
unsigned fsr1_size_builds(const Fsr1Pass *p);

#endif /* SR_FSR1_PASS_H */
