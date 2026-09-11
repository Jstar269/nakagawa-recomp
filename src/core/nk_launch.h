/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_LAUNCH_H
#define NK_LAUNCH_H

#include "nk_types.h"
#include "nk_platform.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Typed runtime provider configuration */
typedef struct {
    int resolution_scale;
    int fps_cap;
    bool fullscreen;
    bool vsync;
    bool benchmark_mode;
    bool diagnostic_mode; /* opt-in fail-closed dispatch validation (SR_DISPATCH_FATAL=1) */
    bool gui_mode;        /* launch with --gui (interactive GUI) or --sched (headless scheduler) */
} NkRuntimeConfig;

typedef struct {
    char executable_path[NK_MAX_PATH];
    char image_path[NK_MAX_PATH];
    char working_directory[NK_MAX_PATH];
    char iso_path[NK_MAX_PATH];
    char prepared_root[NK_MAX_PATH];
    char dataroot_path[NK_MAX_PATH];
    char font_dir[NK_MAX_PATH];
    char title_id[64];
    char disc_id[NK_MAX_DISC_ID_LEN];
    uint32_t base_address;
    uint32_t entry_point;

    /* Runtime configuration */
    NkRuntimeConfig config;

    /* Live process tracking */
    NkProcessHandle process;
    bool is_running;
    int exit_code;
    char last_error[256];
} NkLaunchSession;

/* Prepare a launch session for the given game entry.
 * Validates executable existence, ISO presence, and builds environment/argv.
 */
NkResult nk_launch_prepare_session(
    NkLaunchSession *session,
    const NkGameEntry *game,
    const char *repo_or_install_root
);

/* Start the prepared session, launching the native runtime as a child process. */
NkResult nk_launch_start(NkLaunchSession *session);

/* Check if runtime child process is currently running */
bool nk_launch_is_running(NkLaunchSession *session);

/* Wait for runtime process to exit (timeout_ms < 0 for infinite) */
int nk_launch_wait(NkLaunchSession *session, int timeout_ms);

/* Terminate child process cleanly if running */
void nk_launch_stop(NkLaunchSession *session);

#ifdef __cplusplus
}
#endif

#endif /* NK_LAUNCH_H */
