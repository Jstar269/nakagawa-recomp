/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr3_fg_pass.h - FSR 3.1.6's frame generation on Vulkan: AMD's seventeen compute passes, frame
 * interpolation's ten and optical flow's seven (third_party/fidelityfx_sdk, embedded as
 * fsr3fg_*_comp.inc), run in the order and on the resources that fsr3_fg_host.c plans, and recorded
 * into the caller's command buffer on its queue.
 *
 * Optical flow's search pass sums over 32-lane waves (ffx_opticalflow_compute_optical_flow_v5.h), so
 * its pipeline requires a subgroup size of 32 with full subgroups. A device that cannot give that is
 * refused before anything is created: fsr3_fg_pass_refusal names what it lacks, and
 * fsr3_fg_pass_create creates no pipeline, image or descriptor for it.
 *
 * Every internal image stays in GENERAL. A clear is vkCmdClearColorImage over all mips with AMD's
 * four 32-bit words as they are (so an integer image takes a float clear's bits, as AMD's Vulkan
 * backend writes it), a discard is a transition from UNDEFINED, and a copy is vkCmdCopyImage. Each
 * clear, copy and dispatch waits for every earlier compute and transfer access; AMD's own barriers
 * are covered by that. A storage binding of a mip at or past the image's last takes the last, as
 * AMD's Vulkan backend clamps it.
 *
 * It loads every Vulkan function through vkGetInstanceProcAddr and calls nothing of the runtime.
 */
#ifndef SR_FSR3_FG_PASS_H
#define SR_FSR3_FG_PASS_H

#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>

#include "fsr3_fg_host.h"

/* The line a caller prints, once, before the reason fsr3_fg_pass_refusal or fsr3_fg_pass_create gives. */
#define FSR3FG_PASS_REFUSED_PREFIX "fsr3fg: frame generation is unavailable on this device: "

/* What the passes need from a physical device, as fsr3_fg_pass_query reads it. */
typedef struct {
    uint32_t api_version;               /* the device's; 0 when the instance has no vkGetPhysicalDeviceProperties2 */
    int storage_read_without_format;    /* shaderStorageImageReadWithoutFormat */
    int storage_write_without_format;   /* shaderStorageImageWriteWithoutFormat */
    VkShaderStageFlags subgroup_stages; /* VkPhysicalDeviceSubgroupProperties */
    VkSubgroupFeatureFlags subgroup_operations;
    int size_control_extension;         /* VK_EXT_subgroup_size_control is offered */
    int size_control, full_subgroups;   /* the subgroupSizeControl and computeFullSubgroups features */
    uint32_t min_subgroup_size, max_subgroup_size;
    VkShaderStageFlags required_size_stages;
    uint32_t max_workgroup_invocations, max_workgroup_subgroups;
    uint32_t max_per_stage[5];          /* uniform buffers, samplers, sampled images, storage images, storage buffers */
    uint32_t max_per_stage_resources;
    char format_missing[96];            /* "" when every format has the features the passes use */
} Fsr3FgDeviceFacts;

/* Reads the facts. `color_format` is the format of the present and HUD-less colour and of the output. */
void fsr3_fg_pass_query(PFN_vkGetInstanceProcAddr get_instance_proc_addr, VkInstance instance,
                        VkPhysicalDevice physical_device, VkFormat color_format, Fsr3FgDeviceFacts *facts);

/* NULL when the facts allow the passes, otherwise what is missing, the first in this order: Vulkan 1.1,
 * storage images without a format, subgroup arithmetic in compute shaders, subgroup size control
 * (Vulkan 1.3 or VK_EXT_subgroup_size_control) with its two features, a required size of 32 in compute,
 * 256 invocations a workgroup, the descriptor counts a pass binds, and the format features. Pure: it
 * reads only `facts`. */
const char *fsr3_fg_pass_refusal(const Fsr3FgDeviceFacts *facts);

typedef struct Fsr3FgPass Fsr3FgPass;

typedef struct {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    /* Descriptor sets and constants are kept per slot; a slot is reused once its last use finished.
     * Each use of a slot takes at most one prepare and one dispatch. */
    uint32_t frames_in_flight;
    /* The flags must include FSR3FG_HOST_ENABLE_DEPTH_INVERTED (fsr3_fg_host_init's rule). The display
     * must be at least 64 x 64: optical flow's smallest input level is the display over 64. */
    Fsr3FgHostDesc desc;
    VkFormat color_format;
    /* What fsr3_fg_pass_query found for this physical device and `color_format`. */
    const Fsr3FgDeviceFacts *facts;
    /* The device was created with shaderStorageImageReadWithoutFormat, shaderStorageImageWriteWithoutFormat,
     * subgroupSizeControl and computeFullSubgroups enabled (and VK_EXT_subgroup_size_control below 1.3). */
    int features_enabled;
} Fsr3FgPassInit;

/* NULL, with the reason, when the device is refused (fsr3_fg_pass_refusal's text, with nothing
 * created), the description is refused, or a Vulkan object cannot be made. */
Fsr3FgPass *fsr3_fg_pass_create(const Fsr3FgPassInit *init, char *error, size_t error_size);
/* Waits for the device to go idle, then frees everything. NULL is fine. */
void fsr3_fg_pass_destroy(Fsr3FgPass *p);

/* Bytes of device memory the internal and shared images and the counters take. */
uint64_t fsr3_fg_pass_memory_bytes(const Fsr3FgPass *p);

/* ffxConfigureDescFrameGeneration: the frame's ID, and whether its dispatch passes a HUD-less colour. */
void fsr3_fg_pass_configure(Fsr3FgPass *p, uint64_t frame_id, int has_hudless);

typedef struct {
    /* The game's depth and motion vectors, at render size, in `input_layout` (SHADER_READ_ONLY_OPTIMAL or
     * GENERAL), their writes made visible to compute-shader reads by the caller. */
    VkImageView depth, motion_vectors;
    VkImageLayout input_layout;
    Fsr3FgHostPrepare params;
    uint32_t slot; /* below frames_in_flight */
} Fsr3FgPassPrepare;

typedef struct {
    /* The real frame as presented, at display size in `color_format`, in GENERAL: sampled and copied
     * from, so it needs SAMPLED and TRANSFER_SRC usage. Its writes made visible to compute-shader and
     * transfer reads by the caller. */
    VkImage present_image;
    VkImageView present;
    /* The same frame before its HUD, the same way, exactly when the frame was configured with one;
     * otherwise VK_NULL_HANDLE. */
    VkImage hudless_image;
    VkImageView hudless;
    /* A storage view of the generated image, at display size in `color_format`, in GENERAL (STORAGE and
     * SAMPLED usage). The caller orders its next use after COMPUTE_SHADER / SHADER_WRITE. */
    VkImageView output;
    Fsr3FgHostDispatch params;
    uint32_t slot;
} Fsr3FgPassDispatch;

/* Record the provider's prepare and dispatch. 0, or -1 with nothing recorded and the context as it was
 * when the call is refused (a missing view, a bad slot or layout, a HUD-less colour that does not match
 * the configure, or what fsr3_fg_host refuses). */
int fsr3_fg_pass_prepare(Fsr3FgPass *p, VkCommandBuffer cmd, const Fsr3FgPassPrepare *f);
int fsr3_fg_pass_dispatch(Fsr3FgPass *p, VkCommandBuffer cmd, const Fsr3FgPassDispatch *f);

/* The plan behind the last recorded call, for tests. */
const Fsr3FgHost *fsr3_fg_pass_host(const Fsr3FgPass *p);

#endif /* SR_FSR3_FG_PASS_H */
