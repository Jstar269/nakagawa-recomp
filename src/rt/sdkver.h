// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Pure retained compiled-SDK-version state.
 *
 * Every sceKernelSetCompiledSdkVersion* firmware variant must update the same
 * retained value that SDK-dependent consumers later read (issue #71). Kept
 * free of any runtime dependency (no CpuState, no memory access) so the
 * retained-state contract can be regression-tested standalone
 * (src/rt/sdkver_selftest.c) without mocking the HLE layer. */

#ifndef SR_SDKVER_H
#define SR_SDKVER_H

#include <stdint.h>

/* The devkit version word that sceKernelDevkitVersion (NID 0x3fc9ae6a) returns for the modelled
 * firmware. This is the one definition: the HLE handler returns it, and the native selftest pins the
 * measured literal so either one drifting fails. Measured on the PSP-3000 6.61 oracle 2026-10-15 (record PSP-SYSTEM-001, case model-profile). It is
 * a firmware fact, not the title's retained compiled-SDK version above. */
#define SR_PSP_FIRMWARE_DEVKIT_VERSION 0x06060110u

/* Store the version a Set variant supplied. All shipping variants report
 * success; the return value is the HLE handler result. */
static inline uint32_t sr_sdkver_set(uint32_t *state, uint32_t version) {
    *state = version;
    return 0;
}

/* Read the retained version an SDK-dependent consumer would observe. */
static inline uint32_t sr_sdkver_get(const uint32_t *state) {
    return *state;
}

#endif /* SR_SDKVER_H */
