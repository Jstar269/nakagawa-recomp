/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/*
 * Process start and end for a title's runtime library on Android (libnk_title_<id>.so).
 *
 * TitleActivity runs in its own app process (":title") and calls nk_android_title_main() with the
 * argv the launcher built, exactly what a desktop child process gets. This file adds what the
 * desktop gets from the operating system for free:
 *   - the pid, in NK_ANDROID_PID_FILE, so the launcher can follow a process that is not its child;
 *   - the exit status, in NK_ANDROID_EXIT_FILE, from a return out of main() or from exit(),
 *     _exit() or _Exit() anywhere in the runtime (the library is linked with --wrap for those);
 *   - the end of the process when the run ends, as on the desktop: the runtime's global state
 *     is never reused by a second run in the same process.
 * See nk_platform_spawn_process in src/core/nk_platform_posix.c for the launcher side.
 */

#include "nk_platform.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv);   /* src/rt/driver.c */

void __real_exit(int status) __attribute__((noreturn));
void __real__exit(int status) __attribute__((noreturn));
void __real__Exit(int status) __attribute__((noreturn));

static void write_number_file(const char *env_name, long value) {
    const char *path = getenv(env_name);
    char text[32];
    if (!path || !path[0]) return;
    int length = snprintf(text, sizeof(text), "%ld\n", value);
    if (length <= 0) return;
    /* Written to a temporary name and renamed, so the launcher never reads half a number. */
    char temp[1024];
    if (snprintf(temp, sizeof(temp), "%s.tmp", path) >= (int)sizeof(temp)) return;
    int fd = open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return;
    ssize_t written = write(fd, text, (size_t)length);
    close(fd);
    if (written == (ssize_t)length) (void)rename(temp, path);
}

static void record_exit(int status) {
    static int recorded;
    if (recorded) return;
    recorded = 1;
    fflush(stdout);
    fflush(stderr);
    write_number_file("NK_ANDROID_EXIT_FILE", (long)(status & 0xff));
}

void __wrap_exit(int status) {
    record_exit(status);
    __real_exit(status);
}

void __wrap__exit(int status) {
    record_exit(status);
    __real__exit(status);
}

void __wrap__Exit(int status) {
    record_exit(status);
    __real__Exit(status);
}

__attribute__((visibility("default"))) int nk_android_title_main(int argc, char **argv) {
    write_number_file("NK_ANDROID_PID_FILE", (long)getpid());
    (void)nk_platform_host_startup("nakagawa-title");
    int status = main(argc, argv);
    record_exit(status);
    __real_exit(status);
}
