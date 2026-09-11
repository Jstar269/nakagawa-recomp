/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* POSIX process backend: wait timeouts and exit-status reporting.
 *
 * nk_platform.h reserves a negative timeout for an infinite wait, and the
 * Win32 backend has always honoured finite ones through WaitForSingleObject.
 * The POSIX backend discarded the value and always blocked, so a caller such
 * as nk_launch_wait(session, 5000) hung forever whenever the runtime stalled.
 * Win32 has tests/native/test_win32_process.c; this is the equivalent for the
 * other backend, and only builds where that backend does.
 */

#if defined(_WIN32) || defined(_WIN64)

#include <stdio.h>

int main(void) {
    printf("[POSIX_PROCESS_TEST] Skipped: this backend is not built on Windows.\n");
    return 0;
}

#else

/* Strict -std=c99 hides clock_gettime, nanosleep and CLOCK_MONOTONIC behind
   the feature-test macro, exactly as src/core/nk_platform_posix.c does. */
#define _POSIX_C_SOURCE 200809L

#include "nk_platform.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static double elapsed_ms(const struct timespec *a, const struct timespec *b) {
    return (double)(b->tv_sec - a->tv_sec) * 1000.0
         + (double)(b->tv_nsec - a->tv_nsec) / 1000000.0;
}

int main(void) {
    struct timespec t0, t1;

    /* 1. A finite timeout must expire and return, leaving the child alive.
     *
     * Before the fix this blocked for the child's full 30 seconds and then
     * returned sleep's exit status, so both assertions below failed. */
    printf("[POSIX_PROCESS_TEST] Subtest 1: a finite timeout expires\n");
    fflush(stdout);

    NkProcessHandle slow;
    memset(&slow, 0, sizeof(slow));
    const char *slow_argv[] = { "/bin/sleep", "30", NULL };
    assert(nk_platform_spawn_process("/bin/sleep", slow_argv, NULL, NULL, &slow));

    assert(clock_gettime(CLOCK_MONOTONIC, &t0) == 0);
    int timed_out = nk_platform_wait_process(&slow, 300);
    assert(clock_gettime(CLOCK_MONOTONIC, &t1) == 0);

    assert(timed_out == -1);
    assert(elapsed_ms(&t0, &t1) < 5000.0);
    /* The handle is still usable: a timeout is not a reap. */
    assert(nk_platform_is_process_running(&slow));

    nk_platform_terminate_process(&slow);
    nk_platform_close_process(&slow);

    /* 2. A child that finishes inside the timeout reports its real code. */
    printf("[POSIX_PROCESS_TEST] Subtest 2: exit code within the timeout\n");
    fflush(stdout);

    NkProcessHandle quick;
    memset(&quick, 0, sizeof(quick));
    const char *quick_argv[] = { "/bin/sh", "-c", "exit 7", NULL };
    assert(nk_platform_spawn_process("/bin/sh", quick_argv, NULL, NULL, &quick));
    assert(nk_platform_wait_process(&quick, 10000) == 7);
    nk_platform_close_process(&quick);

    /* 3. A negative timeout still means an unbounded wait. */
    printf("[POSIX_PROCESS_TEST] Subtest 3: negative timeout waits\n");
    fflush(stdout);

    NkProcessHandle blocking;
    memset(&blocking, 0, sizeof(blocking));
    const char *blocking_argv[] = { "/bin/sh", "-c", "exit 5", NULL };
    assert(nk_platform_spawn_process("/bin/sh", blocking_argv, NULL, NULL, &blocking));
    assert(nk_platform_wait_process(&blocking, -1) == 5);
    nk_platform_close_process(&blocking);

    /* 4. A child already reaped by the running-check still reports its code.
     *
     * A POSIX child can be reaped exactly once. nk_platform_is_process_running
     * reaps it to learn that it exited, which used to throw the status away and
     * leave nk_platform_wait_process with nothing but ECHILD -- so the player,
     * which polls before waiting, printed "exited with code -1" for every run
     * on POSIX regardless of what the runtime actually returned. */
    printf("[POSIX_PROCESS_TEST] Subtest 4: status survives the running-check\n");
    fflush(stdout);

    NkProcessHandle polled;
    memset(&polled, 0, sizeof(polled));
    const char *polled_argv[] = { "/bin/sh", "-c", "exit 3", NULL };
    assert(nk_platform_spawn_process("/bin/sh", polled_argv, NULL, NULL, &polled));

    bool observed_exit = false;
    for (int i = 0; i < 2000; i++) {
        if (!nk_platform_is_process_running(&polled)) {
            observed_exit = true;
            break;
        }
        struct timespec slice = { 0, 5L * 1000L * 1000L }; /* 5 ms */
        nanosleep(&slice, NULL);
    }
    assert(observed_exit);
    assert(nk_platform_wait_process(&polled, 0) == 3);
    nk_platform_close_process(&polled);

    printf("[POSIX_PROCESS_TEST] ALL POSIX PROCESS TESTS PASSED!\n");
    return 0;
}

#endif /* !_WIN32 */
