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
    /* Surface-format pick (issue #884): VK_INCOMPLETE (5) is a success code, so the first 32
     * formats of a 40-format surface still count; BGRA8 UNORM (44) wins when present, else
     * entry 0; zero formats, NULL lists and failed queries are refused. */
    {
        enum { FMT_R8G8B8A8_UNORM = 37, FMT_BGRA8_UNORM = 44 };
        enum { RES_SUCCESS = 0, RES_INCOMPLETE = 5, RES_OUT_OF_HOST_MEMORY = -1 };
        static const int two_bgra[] = { 37, 44, 37, 44 };
        static const int no_bgra[] = { 37, 50 };
        int big[40];
        for (int i = 0; i < 40; i++) big[i] = FMT_R8G8B8A8_UNORM;
        big[20] = FMT_BGRA8_UNORM;
        assert(sdl3vk_pick_surface_format(RES_INCOMPLETE, 32, big) == 20);
        big[20] = FMT_R8G8B8A8_UNORM;
        big[35] = FMT_BGRA8_UNORM;   /* past the 32-entry cap: never seen */
        assert(sdl3vk_pick_surface_format(RES_INCOMPLETE, 40, big) == 0);
        assert(sdl3vk_pick_surface_format(RES_INCOMPLETE, 35, big) == 0);
        assert(sdl3vk_pick_surface_format(RES_SUCCESS, 4, two_bgra) == 1);  /* first match */
        assert(sdl3vk_pick_surface_format(RES_SUCCESS, 2, no_bgra) == 0);   /* fallback */
        assert(sdl3vk_pick_surface_format(RES_INCOMPLETE, 2, no_bgra) == 0);
        assert(sdl3vk_pick_surface_format(RES_SUCCESS, 0, two_bgra) == -1); /* zero count */
        assert(sdl3vk_pick_surface_format(RES_INCOMPLETE, 0, two_bgra) == -1);
        assert(sdl3vk_pick_surface_format(RES_SUCCESS, 2, NULL) == -1);
        assert(sdl3vk_pick_surface_format(RES_OUT_OF_HOST_MEMORY, 4, two_bgra) == -1);
    }
    /* Present scaling (issue #1129): destination rectangle, prescale factor and final filter
     * for each mode. An exact integer fit is one nearest blit in every mode. */
    {
#define PLAN_IS(p, X, Y, W, H, PRE, NEAR) \
    ((p).x == (X) && (p).y == (Y) && (p).w == (W) && (p).h == (H) && \
     (p).prescale == (PRE) && (p).nearest == (NEAR))
        enum { SHARP = SDL3VK_SCALING_SHARP, INTEGER = SDL3VK_SCALING_INTEGER,
               BILINEAR = SDL3VK_SCALING_BILINEAR };
        assert(sdl3vk_parse_present_scaling(NULL) == SHARP);
        assert(sdl3vk_parse_present_scaling("") == SHARP);
        assert(sdl3vk_parse_present_scaling("sharp") == SHARP);
        assert(sdl3vk_parse_present_scaling("integer") == INTEGER);
        assert(sdl3vk_parse_present_scaling("bilinear") == BILINEAR);
        assert(sdl3vk_parse_present_scaling("Sharp") == -1);
        assert(sdl3vk_parse_present_scaling("linear") == -1);
        static const int modes[] = { SHARP, INTEGER, BILINEAR };
        for (int m = 0; m < 3; m++) {
            Sdl3VkPresentPlan p;
            p = sdl3vk_present_plan(modes[m], 480, 272, 480, 272, 4096);   /* 1x: unchanged */
            assert(PLAN_IS(p, 0, 0, 480, 272, 1, 1));
            p = sdl3vk_present_plan(modes[m], 480, 272, 960, 544, 4096);   /* 2x */
            assert(PLAN_IS(p, 0, 0, 960, 544, 1, 1));
            p = sdl3vk_present_plan(modes[m], 480, 272, 1440, 816, 4096);  /* 3x */
            assert(PLAN_IS(p, 0, 0, 1440, 816, 1, 1));
            p = sdl3vk_present_plan(modes[m], 480, 272, 1920, 1088, 4096); /* 4x */
            assert(PLAN_IS(p, 0, 0, 1920, 1088, 1, 1));
            p = sdl3vk_present_plan(modes[m], 0, 0, 960, 544, 4096);       /* nothing to draw */
            assert(p.w == 0 && p.h == 0 && p.prescale == 1);
            p = sdl3vk_present_plan(modes[m], 480, 272, 0, 544, 4096);
            assert(p.w == 0 && p.h == 0 && p.prescale == 1);
        }
        Sdl3VkPresentPlan p;
        /* 1080p fullscreen: 3.97x fits 1905x1080. Sharp enlarges to 4x (1920x1088) with nearest,
         * then shrinks linearly; integer letterboxes 3x; bilinear is one linear blit. */
        p = sdl3vk_present_plan(SHARP, 480, 272, 1920, 1080, 4096);
        assert(PLAN_IS(p, 7, 0, 1905, 1080, 4, 0));
        p = sdl3vk_present_plan(INTEGER, 480, 272, 1920, 1080, 4096);
        assert(PLAN_IS(p, 240, 132, 1440, 816, 1, 1));
        p = sdl3vk_present_plan(BILINEAR, 480, 272, 1920, 1080, 4096);
        assert(PLAN_IS(p, 7, 0, 1905, 1080, 1, 0));
        /* A fractional window. */
        p = sdl3vk_present_plan(SHARP, 480, 272, 1000, 600, 4096);
        assert(PLAN_IS(p, 0, 17, 1000, 566, 3, 0));
        p = sdl3vk_present_plan(INTEGER, 480, 272, 1000, 600, 4096);
        assert(PLAN_IS(p, 20, 28, 960, 544, 1, 1));
        /* Smaller than the source: every mode shrinks with one linear blit. */
        for (int m = 0; m < 3; m++) {
            p = sdl3vk_present_plan(modes[m], 480, 272, 400, 300, 4096);
            assert(PLAN_IS(p, 0, 37, 400, 226, 1, 0));
        }
        /* A 2x render-scaled source: integer multiples are of the source pixels. */
        p = sdl3vk_present_plan(SHARP, 960, 544, 1920, 1080, 4096);
        assert(PLAN_IS(p, 7, 0, 1905, 1080, 2, 0));
        p = sdl3vk_present_plan(INTEGER, 960, 544, 1920, 1080, 4096);
        assert(PLAN_IS(p, 480, 268, 960, 544, 1, 1));
        /* 4K: the 8x prescale needs 3840 pixels; a device limit below that falls back to linear. */
        p = sdl3vk_present_plan(SHARP, 480, 272, 3840, 2160, 4096);
        assert(PLAN_IS(p, 14, 0, 3811, 2160, 8, 0));
        p = sdl3vk_present_plan(SHARP, 480, 272, 3840, 2160, 2048);
        assert(PLAN_IS(p, 14, 0, 3811, 2160, 1, 0));
#undef PLAN_IS
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
