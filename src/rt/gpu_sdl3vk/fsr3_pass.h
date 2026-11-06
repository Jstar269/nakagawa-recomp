/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr3_pass.h - FSR 3.1.6's upscaler on Vulkan: AMD's eight compute passes (third_party/fidelityfx_sdk,
 * embedded as fsr3_*_comp.inc), run in the order and on the resources that fsr3_host.c plans, and
 * recorded into the caller's command buffer on its queue.
 *
 * Every internal image stays in GENERAL. A clear is vkCmdClearColorImage over all mips, and a discard
 * is a transition from UNDEFINED. Each dispatch waits for every earlier compute and transfer write.
 *
 * It loads every Vulkan function through vkGetInstanceProcAddr and calls nothing of the runtime.
 */
#ifndef SR_FSR3_PASS_H
#define SR_FSR3_PASS_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "fsr3_host.h"

typedef struct Fsr3Pass Fsr3Pass;

typedef struct {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    /* Created with shaderStorageImageReadWithoutFormat and shaderStorageImageWriteWithoutFormat. */
    VkDevice device;
    /* Descriptor sets and constants are kept per slot; a slot is reused once its last use finished. */
    uint32_t frames_in_flight;
    /* The flags must include FSR3_HOST_ENABLE_DEPTH_INVERTED: the shaders are built for inverted depth.
     * The maximum render size must be at least 2 x 2 (AMD's half-size images). */
    Fsr3HostDesc desc;
} Fsr3PassInit;

/* NULL when the device can run the passes, otherwise the name of the first missing feature or
 * format support. `tc_format` is the format of the transparency-and-composition input, which the
 * shaders sample with linear filtering, or VK_FORMAT_UNDEFINED when none is ever passed. */
const char *fsr3_pass_unavailable(PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkInstance instance,
                                  VkPhysicalDevice physical_device, VkFormat tc_format);

/* NULL, with the reason, when the description is refused or a Vulkan object cannot be made. */
Fsr3Pass *fsr3_pass_create(const Fsr3PassInit *init, char *error, size_t error_size);
/* Waits for the device to go idle, then frees everything. NULL is fine. */
void fsr3_pass_destroy(Fsr3Pass *p);

/* Bytes of device memory the internal images take. */
uint64_t fsr3_pass_memory_bytes(const Fsr3Pass *p);

typedef struct {
    /* The inputs, at render size, in `input_layout` (SHADER_READ_ONLY_OPTIMAL or GENERAL), their
     * writes made visible to compute-shader reads by the caller. VK_NULL_HANDLE for an absent
     * optional input (exposure, reactive, transparency and composition). */
    VkImageView color, depth, motion_vectors;
    VkImageView exposure, reactive, transparency_and_composition;
    VkImageLayout input_layout;
    /* A storage view of the output, at upscale size, in GENERAL. The pass writes it from compute
     * shaders; the caller orders its next use after COMPUTE_SHADER / SHADER_WRITE. */
    VkImageView output;
    /* The dispatch. Its has_* fields are ignored: they are taken from the views above. */
    Fsr3HostDispatch params;
    uint32_t slot; /* below frames_in_flight */
} Fsr3PassFrame;

/* Records one FSR 3 dispatch. 0, or -1 with nothing recorded when the frame is refused (a missing
 * view, a bad slot or layout, or sizes fsr3_host_dispatch refuses). */
int fsr3_pass_record(Fsr3Pass *p, VkCommandBuffer cmd, const Fsr3PassFrame *f);

/* The plan behind the last recorded dispatch, for tests. */
const Fsr3Host *fsr3_pass_host(const Fsr3Pass *p);

#endif /* SR_FSR3_PASS_H */
