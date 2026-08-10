// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "gpu_sdl3vk/sdl3vk.h"
#include "perf.h"

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
    int rc = sdl3vk_capture_selftest();
    if (rc == 0) return 0;
    if (rc == 77) return 77;
    return 1;
}
