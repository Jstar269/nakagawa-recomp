// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#ifndef NAKAGAWA_GUEST_INTERP_H
#define NAKAGAWA_GUEST_INTERP_H

#include <stdint.h>

#include "recomp.h"

typedef enum SrGuestInterpResult {
    SR_GUEST_INTERP_AOT_HANDOFF       = 1,
    SR_GUEST_INTERP_NOT_EXECUTABLE    = -1,
    SR_GUEST_INTERP_MISALIGNED_PC     = -2,
    SR_GUEST_INTERP_FETCH_BOUNDARY    = -3,
    SR_GUEST_INTERP_UNSUPPORTED       = -4,
    SR_GUEST_INTERP_MEMORY_FAULT      = -5,
    SR_GUEST_INTERP_MISALIGNED_DATA   = -6,
} SrGuestInterpResult;

typedef struct SrGuestInterpFault {
    uint32_t pc;
    uint32_t opcode;
    uint32_t address;
    int opcode_valid;
} SrGuestInterpFault;

/* Execute from entry until control reaches a registered AOT destination or a
 * fail-closed boundary. There is deliberately no instruction-count escape hatch:
 * a guest loop is guest behavior, not permission to fabricate completion. */
SrGuestInterpResult sr_guest_interp_run(
    CpuState *s,
    uint32_t entry,
    SrGuestInterpFault *fault);

const char *sr_guest_interp_result_name(SrGuestInterpResult result);

#endif
