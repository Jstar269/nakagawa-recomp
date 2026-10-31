/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_BACKEND_VK_H
#define NK_UI_BACKEND_VK_H

#include "nk_ui_draw.h"

/* The Vulkan backend for nk_ui (lane C3c), shaped like Dear ImGui's Vulkan
   backend: the host owns the instance, device, queue, render pass and command
   buffers, and the backend records into them.

   The backend is compiled in only when the build defines NK_UI_WITH_VULKAN; then
   this header includes <vulkan/vulkan.h> for NkUiVkInit. Without it every
   function is a stub: nk_ui_vk_available() returns false, nk_ui_vk_create()
   returns NULL and prepare and render return false. The rest of the header names
   no Vulkan type, so Vulkan-free code can include it: the command buffer is
   passed as `struct VkCommandBuffer_T *`, which is what VkCommandBuffer is on
   every platform (a dispatchable handle).

   The backend loads every Vulkan function it calls through the
   vkGetInstanceProcAddr the host passes in, so it needs no link-time Vulkan
   dependency. The device must support Vulkan 1.1 (the embedded shaders are
   SPIR-V 1.3). */

typedef struct NkUiVk NkUiVk;
typedef struct NkUiVkInit NkUiVkInit;
struct VkCommandBuffer_T;

/* Frames in flight one backend can serve (NkUiVkInit.frames_in_flight). */
#define NK_UI_VK_MAX_FRAMES 8u

#if defined(NK_UI_WITH_VULKAN)
#include <vulkan/vulkan.h>

/* Every handle stays owned by the host and must outlive the backend. */
struct NkUiVkInit {
    PFN_vkGetInstanceProcAddr get_instance_proc_addr;
    VkInstance instance;
    VkPhysicalDevice physical_device;
    VkDevice device;
    /* The queue the host submits the backend's command buffers to, and its
       family (which must support graphics). nk_ui_vk_destroy waits for this
       queue to go idle. */
    uint32_t queue_family;
    VkQueue queue;
    /* The pipeline is built for this render pass and subpass, which must have
       one colour attachment. */
    VkRenderPass render_pass;
    uint32_t subpass;
    VkSampleCountFlagBits samples;     /* 0 reads as VK_SAMPLE_COUNT_1_BIT */
    uint32_t frames_in_flight;         /* 1 to NK_UI_VK_MAX_FRAMES */
    VkPipelineCache pipeline_cache;    /* optional */
    const VkAllocationCallbacks *allocator; /* optional */
    /* Shape and raster for NK_UI_DRAW_TEXT (as nk_ui_sdlr_create takes it); its
       `user` pointer must outlive the backend. NULL draws no text. */
    const NkUiTextBackend *text;
};
#endif

/* True when this build includes the Vulkan backend. */
bool nk_ui_vk_available(void);

/* Creates the backend: function pointers, samplers, the pipeline for
   init->render_pass, and nothing per frame yet. Returns NULL for a NULL or
   incomplete init, a device without Vulkan 1.1, a queue family without
   graphics, or any Vulkan failure. */
NkUiVk *nk_ui_vk_create(const NkUiVkInit *init);

/* Waits for init->queue to go idle, then frees everything the backend made. The
   host must not use that queue from another thread meanwhile. NULL is fine. */
void nk_ui_vk_destroy(NkUiVk *vk);

/* Tells the backend the device scale (NkUiScale.density) the next lists use, as
   nk_ui_sdlr_set_scale does: a change drops every glyph atlas page, rebuilt
   lazily by the next prepare. An unchanged scale does nothing. */
void nk_ui_vk_set_scale(NkUiVk *vk, float density);

/* A frame is two calls, because uploads cannot be recorded inside a render pass:

   1. nk_ui_vk_prepare, outside a render pass: validates the list, fills the
      frame slot's host-visible vertex and index buffers, rasterises new glyphs
      into the atlas and records the uploads of new atlas regions, image textures
      and the waves texture into `command_buffer`, with their barriers.
   2. nk_ui_vk_render, inside a render pass begun on init->render_pass (and
      subpass): binds the pipeline, sets the viewport and push constants for a
      width x height framebuffer, and draws every command, each clipped with
      vkCmdSetScissor to its NkUiDrawCmd.clip (which already folds in every
      enclosing scissor) clamped to the framebuffer. A command whose clip is
      empty is skipped. The pipeline, descriptor set, viewport and scissor stay
      bound afterwards; a host that draws more must set its own again.

   frame_slot is in [0, frames_in_flight). Before preparing a slot again the host
   must have waited for the GPU to finish the command buffers recorded with that
   slot last time (its fence), and it submits command buffers to init->queue in
   the order it recorded them. Resources the backend replaces (atlas pages,
   evicted images) are freed only then. One backend prepares one list per slot
   at a time; render draws the list the last prepare took, unchanged, and may be
   called more than once for it.

   Both return false, and record nothing that draws, for a NULL argument or a
   malformed list (an index or command range outside the list, scissors nested
   deeper than NK_UI_DRAW_CLIP_DEPTH, a scissor end with none open), and prepare
   also for an allocation or Vulkan failure and for a list with more than four
   NK_UI_CUSTOM_WAVES commands (each needs its own texture for the frame). render
   returns false when the list is not the one prepared or prepare failed.

   Text, images and waves are drawn as the SDL_Renderer backend draws them: a
   glyph atlas filled through the NkUiTextBackend, image textures cached by
   NkUiImage.serial (and drawn size, when shrunk), the waves through the CPU
   reference at a third of their size. An image whose texture cannot be made is
   left out and counted, not an error; other custom commands are skipped and
   counted. */
bool nk_ui_vk_prepare(NkUiVk *vk, const NkUiDrawList *list,
                      struct VkCommandBuffer_T *command_buffer, uint32_t frame_slot);
bool nk_ui_vk_render(NkUiVk *vk, const NkUiDrawList *list,
                     struct VkCommandBuffer_T *command_buffer,
                     uint32_t width, uint32_t height);

typedef struct {
    uint32_t atlas_pages;       /* pages alive now */
    uint32_t atlas_glyphs;      /* glyph entries cached now, including empty ones */
    uint32_t atlas_rebuilds;    /* times a populated atlas was dropped */
    uint32_t atlas_uploads;     /* glyph bitmaps copied into a page, ever */
    uint32_t glyphs_drawn;      /* glyph quads prepared, ever */
    uint32_t text_runs_drawn;   /* text commands prepared, ever */
    uint32_t custom_skipped;    /* custom commands skipped, ever */
    uint32_t waves_drawn;       /* waves commands prepared, ever */
    uint32_t waves_rendered;    /* waves frames rendered on the CPU, ever */
    uint32_t images_drawn;      /* image commands prepared, ever */
    uint32_t image_uploads;     /* image textures created, ever */
    uint32_t images_failed;     /* image commands left out for want of a texture, ever */
    uint32_t commands_clipped;  /* drawing commands skipped for an empty clip, ever */
    uint32_t draws_recorded;    /* vkCmdDrawIndexed calls recorded, ever */
    uint32_t frames_failed;     /* prepare calls that returned false, ever */
} NkUiVkStats;

void nk_ui_vk_get_stats(const NkUiVk *vk, NkUiVkStats *out);

#endif /* NK_UI_BACKEND_VK_H */
