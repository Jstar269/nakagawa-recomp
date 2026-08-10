// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
// fbcap_policy.c - frame-capture slot policy (issue #57)
//
// Pure policy, no Vulkan/SDL dependencies: unit-testable without a GPU (exercised by
// gpu_capture_selftest.c before any Vulkan object exists).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fbcap_policy.h"

int sr_fbcap_env_on(const char *name) {
    if (!name) return 0;
    const char *v = getenv(name);
    if (!v) return 0;
    return strtol(v, NULL, 10) != 0;
}

int sr_fbcap_owner(int fbdu, int fbsnap) {
    if (fbdu) return SR_FBCAP_FBDUMP;
    if (fbsnap) return SR_FBCAP_FBSNAP;
    return SR_FBCAP_NONE;
}

int sr_fbcap_path(int owner, int index, char *out, size_t outsz) {
    if (!out || !outsz) return 0;
    out[0] = '\0';
    int n;
    switch (owner) {
    case SR_FBCAP_FBSNAP:
        n = snprintf(out, outsz, "build/snapshots/frame_%04u.ppm", (unsigned)index);
        break;
    case SR_FBCAP_FBDUMP:
        n = snprintf(out, outsz, "present_source.ppm");
        break;
    default:
        return 0;
    }
    return n > 0 && (size_t)n < outsz;
}

int sr_fbcap_exit_status(int owner, int capture_result) {
    if (owner != SR_FBCAP_FBDUMP) return 0;
    if (capture_result == 1) return 0;
    return 1;
}
