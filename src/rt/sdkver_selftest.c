// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Helper-level retained-state regression for the compiled-SDK-version
 * contract (issue #71).
 *
 * Scope: this selftest executes the pure sdkver.h helpers against a
 * TEST-LOCAL state word and a TEST-LOCAL consumer model. It does NOT execute
 * the production HLE dispatch path, and no production SDK-dependent consumer
 * exists in the runtime yet (g_sdk_version currently has no reader beyond
 * the handler). What it proves is the helper contract: one setter over one
 * state word retains exactly what was last stored, for the same NID set the
 * runtime registers. The production side is proven separately:
 * tools/test_sdkver_c.py uses the fail-closed hle.c manifest extraction to
 * prove every SetCompiledSdkVersion NID routes to the shared handler, and a
 * source guard to prove that handler calls sr_sdkver_set. A variant
 * bypassing the shared helper state (the old 0x1b4217bc -> h_ok defect)
 * fails that routing proof, and the stale-value symptom is what this
 * selftest's checks model.
 *
 * Standalone: no runtime, scheduler, or HLE mocking required. Built and run by
 * tools/test_sdkver_c.py; can also be compiled directly:
 *   gcc -Wall -Wextra -Werror -Isrc/rt -o sdkver_selftest src/rt/sdkver_selftest.c
 * Exit code 0 = all invariants hold. */

#include <stdio.h>

#include "sdkver.h"

static int s_failures = 0;

#define EXPECT(cond) do { \
    if (!(cond)) { \
        s_failures++; \
        fprintf(stderr, "sdkver selftest FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* Test-local retained state word (stands in for hle.c's g_sdk_version;
 * the production word itself is not linked into this selftest). */
static uint32_t s_sdk_version = 0;

/* Test model of the single stateful handler every variant must route to
 * (the version argument stands in for A0; production routing to
 * h_SetCompiledSdkVersion is proven by the manifest checks, not here). */
static uint32_t set_compiled_sdk_version(uint32_t version) {
    return sr_sdkver_set(&s_sdk_version, version);
}

/* Test model of a retained-state consumer. sr_sdkver_get is part of this
 * test model; no production SDK-dependent consumer is executed here. */
static uint32_t consumer_reads_sdk_version(void) {
    return sr_sdkver_get(&s_sdk_version);
}

/* The registered SetCompiledSdkVersion firmware-variant NIDs and the SDK
 * version each firmware family would report. tools/test_sdkver_c.py
 * cross-checks this NID list against the extracted hle.c manifest, so a
 * variant added to the runtime without joining the shared state fails there. */
static const struct { uint32_t nid; uint32_t version; } k_variants[] = {
    { 0x7591C7DBu, 0x06060010u },  /* sceKernelSetCompiledSdkVersion */
    { 0x35669D4Cu, 0x06020010u },  /* sceKernelSetCompiledSdkVersion600_602 */
    { 0x1B4217BCu, 0x06050010u },  /* sceKernelSetCompiledSdkVersion603_605 */
};

static void test_initial_state_is_default(void) {
    EXPECT(consumer_reads_sdk_version() == 0u);
}

static void test_every_variant_updates_the_same_retained_state(void) {
    for (unsigned i = 0; i < sizeof(k_variants) / sizeof(k_variants[0]); i++) {
        uint32_t before = consumer_reads_sdk_version();
        EXPECT(set_compiled_sdk_version(k_variants[i].version) == 0u);
        EXPECT(consumer_reads_sdk_version() == k_variants[i].version);
        /* The update is visible as a state CHANGE, not a coincidental
         * default: each vector differs from the previous retained value. */
        EXPECT(consumer_reads_sdk_version() != before);
    }
}

static void test_state_is_retained_until_the_next_set(void) {
    EXPECT(set_compiled_sdk_version(0x06050010u) == 0u);
    /* Repeated consumer reads observe the same retained value; reading is
     * not destructive. */
    EXPECT(consumer_reads_sdk_version() == 0x06050010u);
    EXPECT(consumer_reads_sdk_version() == 0x06050010u);
    EXPECT(set_compiled_sdk_version(0x03070110u) == 0u);
    EXPECT(consumer_reads_sdk_version() == 0x03070110u);
}

int main(void) {
    test_initial_state_is_default();
    test_every_variant_updates_the_same_retained_state();
    test_state_is_retained_until_the_next_set();
    if (s_failures) {
        fprintf(stderr, "sdkver selftest: %d failure(s)\n", s_failures);
        return 1;
    }
    printf("sdkver selftest: OK\n");
    return 0;
}
