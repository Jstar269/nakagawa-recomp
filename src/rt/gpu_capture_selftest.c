// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "gpu_sdl3vk/sdl3vk.h"
#include "gpu_sdl3vk/ge_gpu.h"
#include "perf.h"

#include <assert.h>
#include <stdio.h>

/* Deterministic present-source capture regression (issue #57): arms captures, drives the
 * production present path with synthetic pixels (CPU framebuffer and GPU render-target
 * paths), and byte-checks the published P6 PPMs. No game binaries, captures, or private
 * inputs are consumed.
 *
 * Exit codes: 0 = all captures byte-exact and validation-clean; 77 = SKIP (no Vulkan or
 * no validation layer installed); 1 = a capture/policy/validation failure. */
int main(void) {
    sr_perf_init();
    /* Settings consumers that need no window: SR_VSYNC picks the swapchain
     * present mode, SR_RESOLUTION_SCALE/SR_GPU_SCALE pick the render scale. */
    {
        static const int fifo_only[] = { 2 };
        static const int fifo_mailbox[] = { 2, 1 };
        static const int fifo_immediate[] = { 2, 0 };
        assert(sdl3vk_pick_present_mode(1, fifo_only, 1) == 2);
        assert(sdl3vk_pick_present_mode(1, fifo_mailbox, 2) == 2);
        assert(sdl3vk_pick_present_mode(0, fifo_mailbox, 2) == 1);
        assert(sdl3vk_pick_present_mode(0, fifo_immediate, 2) == 0);
        assert(sdl3vk_pick_present_mode(0, fifo_only, 1) == 2);
        assert(sdl3vk_pick_present_mode(0, NULL, 0) == 2);
    }
    assert(sr_parse_render_scale(NULL, NULL, 4) == 1);
    assert(sr_parse_render_scale(NULL, "1", 4) == 1);
    assert(sr_parse_render_scale(NULL, "3", 4) == 3);
    assert(sr_parse_render_scale(NULL, "4", 4) == 4);
    assert(sr_parse_render_scale(NULL, "8", 4) == 4);  /* clamp */
    assert(sr_parse_render_scale(NULL, "bogus", 4) == 1);
    assert(sr_parse_render_scale("2", "4", 4) == 2);  /* diagnostic wins */
    assert(sr_parse_render_scale("", "4", 4) == 4);   /* empty falls through */
    int rc = sdl3vk_capture_selftest();
    if (rc == 0) return 0;
    if (rc == 77) return 77;
    return 1;
}
