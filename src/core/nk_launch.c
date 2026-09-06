/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_launch.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Helper to check candidate binary paths */
static bool find_candidate_executable(
    const char *root,
    const char *title_id,
    char *out_path,
    size_t max_len
) {
    char sep = nk_platform_path_separator();
    char cand[NK_MAX_PATH];

    /* Candidate 1: build/hst/hst.exe or build/hst/hst */
    snprintf(cand, sizeof(cand), "%s%cbuild%chst%chst.exe", root, sep, sep, sep);
    if (nk_platform_file_exists(cand)) {
        snprintf(out_path, max_len, "%s", cand);
        return true;
    }
    snprintf(cand, sizeof(cand), "%s%cbuild%chst%chst", root, sep, sep, sep);
    if (nk_platform_file_exists(cand)) {
        snprintf(out_path, max_len, "%s", cand);
        return true;
    }

    /* Candidate 2: build/<title_id>/<title_id>.exe */
    if (title_id && *title_id) {
        snprintf(cand, sizeof(cand), "%s%cbuild%c%s%c%s.exe", root, sep, sep, title_id, sep, title_id);
        if (nk_platform_file_exists(cand)) {
            snprintf(out_path, max_len, "%s", cand);
            return true;
        }
        snprintf(cand, sizeof(cand), "%s%cbuild%c%s%c%s", root, sep, sep, title_id, sep, title_id);
        if (nk_platform_file_exists(cand)) {
            snprintf(out_path, max_len, "%s", cand);
            return true;
        }
    }

    /* Candidate 3: bin/nakagawa_runtime.exe or bin/nakagawa_runtime */
    snprintf(cand, sizeof(cand), "%s%cbin%cnakagawa_runtime.exe", root, sep, sep);
    if (nk_platform_file_exists(cand)) {
        snprintf(out_path, max_len, "%s", cand);
        return true;
    }
    snprintf(cand, sizeof(cand), "%s%cbin%cnakagawa_runtime", root, sep, sep);
    if (nk_platform_file_exists(cand)) {
        snprintf(out_path, max_len, "%s", cand);
        return true;
    }

    /* Candidate 4: hst.exe in root */
    snprintf(cand, sizeof(cand), "%s%chst.exe", root, sep);
    if (nk_platform_file_exists(cand)) {
        snprintf(out_path, max_len, "%s", cand);
        return true;
    }
    snprintf(cand, sizeof(cand), "%s%chst", root, sep);
    if (nk_platform_file_exists(cand)) {
        snprintf(out_path, max_len, "%s", cand);
        return true;
    }

    return false;
}

NkResult nk_launch_prepare_session(
    NkLaunchSession *session,
    const NkGameEntry *game,
    const char *repo_or_install_root
) {
    if (!session || !game) return NK_ERROR_GENERIC;
    memset(session, 0, sizeof(*session));

    const char *root = (repo_or_install_root && *repo_or_install_root) ? repo_or_install_root : ".";
    snprintf(session->working_directory, sizeof(session->working_directory), "%s", root);
    snprintf(session->title_id, sizeof(session->title_id), "%s", game->title_id);
    snprintf(session->disc_id, sizeof(session->disc_id), "%s", game->disc_id);
    snprintf(session->prepared_root, sizeof(session->prepared_root), "%s", game->prepared_root);

    session->config.resolution_scale = 1;
    session->config.fps_cap = 60;
    session->config.fullscreen = false;
    session->config.vsync = true;
    session->config.benchmark_mode = false;
    session->config.diagnostic_mode = false;

    /* 1. Resolve executable */
    if (!find_candidate_executable(root, game->title_id, session->executable_path, sizeof(session->executable_path))) {
        snprintf(session->last_error, sizeof(session->last_error), "Runtime binary not found under root: %s", root);
        return NK_ERROR_FILE_NOT_FOUND;
    }

    /* 2. Resolve ISO path */
    if (nk_platform_file_exists(game->iso_path)) {
        snprintf(session->iso_path, sizeof(session->iso_path), "%s", game->iso_path);
    } else {
        /* Check fallback under prepared_root/disc/game.iso */
        char sep = nk_platform_path_separator();
        char fallback_iso[NK_MAX_PATH + 32];
        snprintf(fallback_iso, sizeof(fallback_iso), "%s%cdisc%cgame.iso", game->prepared_root, sep, sep);
        if (nk_platform_file_exists(fallback_iso)) {
            snprintf(session->iso_path, sizeof(session->iso_path), "%.*s", (int)(sizeof(session->iso_path) - 1), fallback_iso);
        } else {
            snprintf(session->last_error, sizeof(session->last_error), "Game source ISO not found: %.*s", (int)(sizeof(session->last_error) - 30), game->iso_path);
            return NK_ERROR_FILE_NOT_FOUND;
        }
    }

    return NK_OK;
}

NkResult nk_launch_start(NkLaunchSession *session) {
    if (!session || session->executable_path[0] == '\0') {
        return NK_ERROR_GENERIC;
    }

    if (session->is_running) {
        if (nk_launch_is_running(session)) {
            return NK_ERROR_ALREADY_EXISTS;
        }
    }

    /* Build environment variables via runtime provider */
    char env_iso[NK_MAX_PATH + 16];
    char env_fps[32];
    char env_ge[32];
    char env_debug[32];
    char env_scale[32];
    char env_vsync[32];
    char env_fatal[32];

    snprintf(env_iso, sizeof(env_iso), "PSP_ISO=%s", session->iso_path);
    snprintf(env_fps, sizeof(env_fps), "SR_FPS_CAP=%d", session->config.fps_cap);
    snprintf(env_ge, sizeof(env_ge), "SR_GPU_GE=1");
    snprintf(env_debug, sizeof(env_debug), "SR_DEBUG=%s", session->config.benchmark_mode ? "0x20" : "0");
    snprintf(env_scale, sizeof(env_scale), "SR_RESOLUTION_SCALE=%d", session->config.resolution_scale);
    snprintf(env_vsync, sizeof(env_vsync), "SR_VSYNC=%d", session->config.vsync ? 1 : 0);

    const char *envp[16];
    int env_count = 0;
    envp[env_count++] = env_iso;
    envp[env_count++] = env_fps;
    envp[env_count++] = env_ge;
    envp[env_count++] = env_debug;
    envp[env_count++] = env_scale;
    envp[env_count++] = env_vsync;

    if (session->config.diagnostic_mode) {
        snprintf(env_fatal, sizeof(env_fatal), "SR_DISPATCH_FATAL=1");
        envp[env_count++] = env_fatal;
    }
    envp[env_count] = NULL;

    const char *argv[] = {
        session->executable_path,
        NULL
    };

    bool ok = nk_platform_spawn_process(
        session->executable_path,
        argv,
        envp,
        session->working_directory[0] ? session->working_directory : NULL,
        &session->process
    );

    if (!ok) {
        snprintf(session->last_error, sizeof(session->last_error), "Failed to spawn runtime process: %.*s", (int)(sizeof(session->last_error) - 40), session->executable_path);
        session->is_running = false;
        return NK_ERROR_PROCESS_SPAWN;
    }

    session->is_running = true;
    return NK_OK;
}

bool nk_launch_is_running(NkLaunchSession *session) {
    if (!session || !session->is_running) return false;
    bool running = nk_platform_is_process_running(&session->process);
    if (!running) {
        session->is_running = false;
    }
    return running;
}

int nk_launch_wait(NkLaunchSession *session, int timeout_ms) {
    if (!session || !session->is_running) return session ? session->exit_code : -1;
    int code = nk_platform_wait_process(&session->process, timeout_ms);
    session->exit_code = code;
    session->is_running = false;
    return code;
}

void nk_launch_stop(NkLaunchSession *session) {
    if (!session) return;
    if (session->is_running) {
        nk_platform_terminate_process(&session->process);
        session->is_running = false;
    }
    nk_platform_close_process(&session->process);
}
