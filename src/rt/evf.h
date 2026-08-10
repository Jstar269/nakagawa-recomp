// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later

/* Pure PSP sceKernelEventFlag pattern/mode helpers.
 *
 * Kept free of any runtime dependency (no CpuState, no memory access) so the
 * exact PSP semantics can be regression-tested standalone (src/rt/evf_selftest.c)
 * without mocking the HLE layer. Behavior cross-checked against PPSSPP's
 * Core/HLE/sceKernelEventFlag.cpp. */

#ifndef SR_EVF_H
#define SR_EVF_H

#include <stdint.h>

/* Wait/poll mode bits (PSP_EVENT_WAIT*). AND matching is mode 0. */
#define SR_EVF_WAIT_OR        0x01u
#define SR_EVF_WAIT_CLEARALL  0x10u
#define SR_EVF_WAIT_CLEAR     0x20u
#define SR_EVF_WAIT_KNOWN     (SR_EVF_WAIT_OR | SR_EVF_WAIT_CLEARALL | SR_EVF_WAIT_CLEAR)

#define SR_EVF_ERR_ILLEGAL_MODE 0x80020195u  /* SCE_KERNEL_ERROR_ILLEGAL_MODE */
#define SR_EVF_ERR_COND         0x800201afu  /* SCE_KERNEL_ERROR_EVF_COND */
#define SR_EVF_ERR_ILPAT        0x800201b1u  /* SCE_KERNEL_ERROR_EVF_ILPAT */

/* sceKernelClearEventFlag: the argument is the mask of bits to KEEP
 * (currentPattern &= bits), not a mask of bits to remove. */
static inline uint32_t sr_evf_clear_pattern(uint32_t pattern, uint32_t keep_mask) {
    return pattern & keep_mask;
}

static inline int sr_evf_matches(uint32_t pattern, uint32_t bits, uint32_t mode) {
    return (mode & SR_EVF_WAIT_OR) ? ((pattern & bits) != 0)
                                   : ((pattern & bits) == bits);
}

/* Pattern left behind after a SUCCESSFUL wait/poll. outBits must be written
 * from the pre-consume pattern; WAITCLEAR applies before WAITCLEARALL. */
static inline uint32_t sr_evf_consume(uint32_t pattern, uint32_t bits, uint32_t mode) {
    if (mode & SR_EVF_WAIT_CLEAR)    pattern &= ~bits;
    if (mode & SR_EVF_WAIT_CLEARALL) pattern = 0;
    return pattern;
}

/* Argument validation shared by wait and poll: unknown mode bits are an
 * illegal mode, and an all-zero wait pattern can never be satisfied. */
static inline uint32_t sr_evf_check_wait_args(uint32_t bits, uint32_t mode) {
    if (mode & ~SR_EVF_WAIT_KNOWN) return SR_EVF_ERR_ILLEGAL_MODE;
    if (bits == 0)                 return SR_EVF_ERR_ILPAT;
    return 0;
}

/* Poll additionally rejects WAITCLEAR|WAITCLEARALL combined. */
static inline uint32_t sr_evf_check_poll_args(uint32_t bits, uint32_t mode) {
    uint32_t rc = sr_evf_check_wait_args(bits, mode);
    if (rc) return rc;
    if ((mode & (SR_EVF_WAIT_CLEAR | SR_EVF_WAIT_CLEARALL)) ==
        (SR_EVF_WAIT_CLEAR | SR_EVF_WAIT_CLEARALL))
        return SR_EVF_ERR_ILLEGAL_MODE;
    return 0;
}

#endif /* SR_EVF_H */
