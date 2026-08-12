// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the psp-recomp authors

#include "recomp.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

static int s_checks;
static int s_failures;

#define CHECK(cond, ...) do {                                                   \
    s_checks++;                                                                 \
    if (!(cond)) {                                                              \
        s_failures++;                                                           \
        fprintf(stderr, "FAIL: ");                                             \
        fprintf(stderr, __VA_ARGS__);                                           \
        fputc('\n', stderr);                                                    \
    }                                                                           \
} while (0)

int main(void) {
    sr_profile_test_reset();

    /* HST and other zero-based PSP images can legitimately execute PC zero. It must be a
     * normal key, not the profiler table's empty-slot sentinel. */
    sr_profile_block(0);
    sr_profile_block(0);
    CHECK(sr_profile_test_block_count(0) == 2,
          "PC zero count=%" PRIu64 " (expected 2)", sr_profile_test_block_count(0));
    CHECK(sr_profile_test_lookup_drops() == 0,
          "unexpected lookup drop after PC-zero insert");

    sr_profile_block(4);
    CHECK(sr_profile_test_block_count(4) == 1,
          "ordinary PC count=%" PRIu64 " (expected 1)", sr_profile_test_block_count(4));

    /* The hash table deliberately bounds a lookup to 64 probes. Multiples of the table size
     * have identical low hash bits, so this fills one probe window deterministically. */
    for (uint32_t i = 1; i < 64; i++) {
        sr_profile_block(i * 131072u);
    }
    CHECK(sr_profile_test_lookup_drops() == 0,
          "lookup dropped before the 64-probe window was full");

    sr_profile_block(64u * 131072u);
    CHECK(sr_profile_test_lookup_drops() == 1,
          "saturated lookup drops=%" PRIu64 " (expected 1)",
          sr_profile_test_lookup_drops());
    CHECK(sr_profile_test_block_count(64u * 131072u) == 0,
          "dropped key unexpectedly appeared in the table");
    CHECK(sr_profile_test_block_count(0) == 2,
          "collision pressure corrupted the PC-zero entry");

    printf("profiler selftest: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
