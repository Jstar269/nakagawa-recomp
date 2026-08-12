// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/*
 * Exercise the production sr_coro_main() implementation without HLE or the
 * SR_CORO_LIFECYCLE_TEST safety instrumentation.  Before the idempotency fix,
 * every repeated adoption allocated a fresh wrapper and changed the scheduler
 * identity; a tight caller could therefore consume unbounded host memory.
 */

#include "sr_coro.h"

#include <stdio.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

static SIZE_T private_usage(void) {
    PROCESS_MEMORY_COUNTERS_EX counters;
    counters.cb = sizeof(counters);
    if (!GetProcessMemoryInfo(GetCurrentProcess(),
                              (PROCESS_MEMORY_COUNTERS *)&counters,
                              sizeof(counters))) {
        return (SIZE_T)-1;
    }
    return counters.PrivateUsage;
}
#endif

int main(void) {
    SrCoro *main_coro = sr_coro_main();
    if (!main_coro) {
        fprintf(stderr, "coro selftest: initial adoption failed\n");
        return 1;
    }

#if defined(_WIN32)
    SIZE_T private_before = private_usage();
    if (private_before == (SIZE_T)-1) {
        fprintf(stderr, "coro selftest: cannot query initial private memory\n");
        return 1;
    }
#endif

    for (unsigned i = 0; i < 1000000u; i++) {
        SrCoro *again = sr_coro_main();
        if (again != main_coro || sr_coro_current() != main_coro) {
            fprintf(stderr,
                    "coro selftest: adoption changed identity at iteration %u\n",
                    i);
            return 1;
        }
    }

#if defined(_WIN32)
    SIZE_T private_after = private_usage();
    if (private_after == (SIZE_T)-1) {
        fprintf(stderr, "coro selftest: cannot query final private memory\n");
        return 1;
    }
    SIZE_T growth = private_after >= private_before ? private_after - private_before : 0;
    if (growth > (SIZE_T)(1u << 20)) {
        fprintf(stderr,
                "coro selftest: repeated adoption grew private memory by %llu bytes\n",
                (unsigned long long)growth);
        return 1;
    }
    printf("coro selftest: private-memory growth=%llu bytes\n",
           (unsigned long long)growth);
#endif

    puts("coro selftest: OK (1000000 repeated adoptions, one identity)");
    return 0;
}
