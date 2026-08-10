// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Focused regression tests for the pure sceKernelEventFlag semantics in evf.h.
 *
 * Standalone: no runtime, scheduler, or HLE mocking required. Built and run by
 * tools/test_evf_c.py; can also be compiled directly:
 *   gcc -Wall -Wextra -Werror -Isrc/rt -o evf_selftest src/rt/evf_selftest.c
 * Exit code 0 = all invariants hold. */

#include <stdio.h>

#include "evf.h"

static int s_failures = 0;

#define EXPECT(cond) do { \
    if (!(cond)) { \
        s_failures++; \
        fprintf(stderr, "evf selftest FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

static void test_clear_is_keep_mask(void) {
    /* Issue #4 acceptance case: ClearEventFlag(0xF0F0, 0x0FF0) leaves 0x00F0. */
    EXPECT(sr_evf_clear_pattern(0xF0F0u, 0x0FF0u) == 0x00F0u);
    EXPECT(sr_evf_clear_pattern(0xFFFFFFFFu, 0u) == 0u);
    EXPECT(sr_evf_clear_pattern(0x12345678u, 0xFFFFFFFFu) == 0x12345678u);
    /* The old (inverted) semantics would have produced pattern & ~bits. */
    EXPECT(sr_evf_clear_pattern(0xF0F0u, 0x0FF0u) != (0xF0F0u & ~0x0FF0u));
}

static void test_and_or_matching(void) {
    /* AND (mode 0): every requested bit must be set. */
    EXPECT(sr_evf_matches(0x0Fu, 0x05u, 0));
    EXPECT(!sr_evf_matches(0x0Au, 0x05u, 0));
    EXPECT(!sr_evf_matches(0x04u, 0x05u, 0));
    /* OR: any requested bit suffices. */
    EXPECT(sr_evf_matches(0x04u, 0x05u, SR_EVF_WAIT_OR));
    EXPECT(sr_evf_matches(0x01u, 0x05u, SR_EVF_WAIT_OR));
    EXPECT(!sr_evf_matches(0xF0u, 0x05u, SR_EVF_WAIT_OR));
    /* Clear bits do not change the matching rule itself. */
    EXPECT(sr_evf_matches(0x0Fu, 0x05u, SR_EVF_WAIT_CLEAR));
    EXPECT(sr_evf_matches(0x04u, 0x05u, SR_EVF_WAIT_OR | SR_EVF_WAIT_CLEARALL));
}

static void test_consume_on_success(void) {
    /* No clear bits: pattern untouched. */
    EXPECT(sr_evf_consume(0xFFu, 0x0Fu, 0) == 0xFFu);
    EXPECT(sr_evf_consume(0xFFu, 0x0Fu, SR_EVF_WAIT_OR) == 0xFFu);
    /* WAITCLEAR removes only the waited bits. */
    EXPECT(sr_evf_consume(0xFFu, 0x0Fu, SR_EVF_WAIT_CLEAR) == 0xF0u);
    EXPECT(sr_evf_consume(0xFFu, 0x0Fu, SR_EVF_WAIT_OR | SR_EVF_WAIT_CLEAR) == 0xF0u);
    /* WAITCLEARALL zeroes the whole pattern. */
    EXPECT(sr_evf_consume(0xFFu, 0x0Fu, SR_EVF_WAIT_CLEARALL) == 0u);
    EXPECT(sr_evf_consume(0xFFu, 0x0Fu, SR_EVF_WAIT_OR | SR_EVF_WAIT_CLEARALL) == 0u);
    /* Both together (reachable via wait, not poll) still ends at zero. */
    EXPECT(sr_evf_consume(0xFFu, 0x0Fu, SR_EVF_WAIT_CLEAR | SR_EVF_WAIT_CLEARALL) == 0u);
}

static void test_wait_arg_validation(void) {
    EXPECT(sr_evf_check_wait_args(0x1u, 0) == 0);
    EXPECT(sr_evf_check_wait_args(0x1u, SR_EVF_WAIT_OR | SR_EVF_WAIT_CLEAR) == 0);
    /* Both clear bits are legal for wait... */
    EXPECT(sr_evf_check_wait_args(0x1u, SR_EVF_WAIT_CLEAR | SR_EVF_WAIT_CLEARALL) == 0);
    /* ...but unknown mode bits and a zero pattern are not. */
    EXPECT(sr_evf_check_wait_args(0x1u, 0x02u) == SR_EVF_ERR_ILLEGAL_MODE);
    EXPECT(sr_evf_check_wait_args(0x1u, 0x100u) == SR_EVF_ERR_ILLEGAL_MODE);
    EXPECT(sr_evf_check_wait_args(0u, 0) == SR_EVF_ERR_ILPAT);
    EXPECT(sr_evf_check_wait_args(0u, SR_EVF_WAIT_OR) == SR_EVF_ERR_ILPAT);
    /* Unknown mode takes precedence over the zero-pattern check (PPSSPP order). */
    EXPECT(sr_evf_check_wait_args(0u, 0x02u) == SR_EVF_ERR_ILLEGAL_MODE);
}

static void test_poll_arg_validation(void) {
    EXPECT(sr_evf_check_poll_args(0x1u, 0) == 0);
    EXPECT(sr_evf_check_poll_args(0x1u, SR_EVF_WAIT_OR | SR_EVF_WAIT_CLEAR) == 0);
    EXPECT(sr_evf_check_poll_args(0x1u, SR_EVF_WAIT_CLEARALL) == 0);
    /* Poll rejects CLEAR and CLEARALL combined. */
    EXPECT(sr_evf_check_poll_args(0x1u, SR_EVF_WAIT_CLEAR | SR_EVF_WAIT_CLEARALL)
           == SR_EVF_ERR_ILLEGAL_MODE);
    /* Shared wait validation still applies. */
    EXPECT(sr_evf_check_poll_args(0x1u, 0x40u) == SR_EVF_ERR_ILLEGAL_MODE);
    EXPECT(sr_evf_check_poll_args(0u, 0) == SR_EVF_ERR_ILPAT);
}

static void test_error_codes_are_psp_values(void) {
    EXPECT(SR_EVF_ERR_ILLEGAL_MODE == 0x80020195u);
    EXPECT(SR_EVF_ERR_COND == 0x800201afu);
    EXPECT(SR_EVF_ERR_ILPAT == 0x800201b1u);
}

static void test_poll_success_flow(void) {
    /* Model the handler's success path: outBits sees the pre-consume pattern,
     * then WAITCLEAR/WAITCLEARALL apply — identically for wait and poll. */
    uint32_t pattern = 0xF0F0u, bits = 0x00F0u, mode = SR_EVF_WAIT_CLEAR;
    EXPECT(sr_evf_check_poll_args(bits, mode) == 0);
    EXPECT(sr_evf_matches(pattern, bits, mode));
    uint32_t out_bits = pattern;
    pattern = sr_evf_consume(pattern, bits, mode);
    EXPECT(out_bits == 0xF0F0u);
    EXPECT(pattern == 0xF000u);

    pattern = 0xF0F0u; mode = SR_EVF_WAIT_OR | SR_EVF_WAIT_CLEARALL;
    EXPECT(sr_evf_matches(pattern, bits, mode));
    out_bits = pattern;
    pattern = sr_evf_consume(pattern, bits, mode);
    EXPECT(out_bits == 0xF0F0u);
    EXPECT(pattern == 0u);
}

int main(void) {
    test_clear_is_keep_mask();
    test_and_or_matching();
    test_consume_on_success();
    test_wait_arg_validation();
    test_poll_arg_validation();
    test_error_codes_are_psp_values();
    test_poll_success_flow();
    if (s_failures) {
        fprintf(stderr, "evf selftest: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("evf selftest: OK\n");
    return 0;
}
