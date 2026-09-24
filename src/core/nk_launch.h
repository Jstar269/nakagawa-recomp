/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_LAUNCH_H
#define NK_LAUNCH_H

#include "nk_types.h"
#include "nk_platform.h"
#include "nk_title_manifest.h"
#include "generated/nk_title_catalog.h"
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
    bool is_elf;
    bool is_psp_container;
    bool entry_in_executable_segment;
    bool has_bss;
    uint32_t load_base;
    uint32_t image_end;
    uint32_t file_backed_end;
    uint32_t bss_start;
    uint32_t bss_end;
    uint16_t program_header_count;
    uint16_t load_segment_count;
} NkLaunchExecutableInfo;

typedef struct {
    char executable_path[NK_MAX_PATH];
    char image_path[NK_MAX_PATH];
    /* Promoted staging payload, when the library entry came from the setup
       wizard. This is the source executable, not the generated flat image. */
    char staged_executable_path[NK_MAX_PATH];
    char working_directory[NK_MAX_PATH];
    char iso_path[NK_MAX_PATH];
    char prepared_root[NK_MAX_PATH];
    char dataroot_path[NK_MAX_PATH];
    char font_dir[NK_MAX_PATH];
    char user_data_root[NK_MAX_PATH];
    /* Memory Stick root handed to the runtime as SR_MEMSTICK. Resolved by
       nk_launch_prepare_session to a directory that is known to be writable;
       a caller with a user-configured save location may overwrite it with an
       absolute path before nk_launch_start. Never empty after a successful
       prepare: the runtime must not be told to write saves somewhere it
       cannot. */
    char memstick_root[NK_MAX_PATH];
    char title_id[64];
    char disc_id[NK_MAX_DISC_ID_LEN];
    char selected_executable[NK_MAX_EXECUTABLE_PATH];
    uint32_t base_address;
    uint32_t entry_point;
    bool package_launch;
    bool experimental_package;
    bool staged_executable_checked;
    NkLaunchExecutableInfo staged_executable_info;

    /* Runtime configuration */
    NkRuntimeConfig config;

    /* Live process tracking */
    NkProcessHandle process;
    bool is_running;
    int exit_code;
    /* Exact launch-mode evidence recorded while nk_launch_start builds argv. */
    bool argv_has_gui;
    char last_error[2048];
} NkLaunchSession;

/* True when the launcher's executable search can resolve a runtime for `title_id`
 * under the root directory `root`. Requires `title_id` to name a validated
 * catalog entry and probes only that entry's identity-derived candidates (the
 * shared contract in generated/nk_title_catalog.h); an unknown title, or a
 * workspace holding only another title's build, is not "available". This is
 * the same probe used by nk_launch_prepare_session, so UI readiness cannot
 * drift from the eventual launch path. */
bool nk_launch_runtime_available(const char *root, const char *title_id);

/* True only when the selected title's identity-matched runtime executable and
 * generated image are both available under `root`. */
bool nk_launch_runtime_package_available(const char *root, const char *title_id);

/* Validate the v1 package discovered for a concrete player library entry.
 * The root is the per-user data root (or an explicit test/user override). */
NkRuntimePackageStatus nk_launch_validate_runtime_package(
    const char *user_data_root,
    const NkGameEntry *game,
    NkRuntimePackageInfo *out_info,
    char *reason,
    size_t reason_size
);

/* Validate a promoted staging EBOOT.BIN against the selected title manifest.
 * Plain ELF32/MIPS files receive full program-header, load-range, entry, and
 * BSS checks. A PSP ~PSP container is recognized as a bounded encrypted/source
 * container; its inner ELF cannot be checked until a lawful decryption phase
 * supplies it. */
NkResult nk_launch_validate_staged_executable(const NkGameEntry *game,
                                              const NkTitleEntry *manifest,
                                              NkLaunchExecutableInfo *out_info,
                                              char *error_message,
                                              size_t error_message_size);

/* Prepare a launch session for the given game entry under the root directory
 * `repo_or_install_root` (a file path is not accepted as an explicit-legacy
 * escape). The session's disc_id/title_id must resolve to ONE agreeing
 * validated catalog entry; the runtime executable and image are resolved only
 * from that entry's identity-derived candidates (no sibling-title or
 * retail-title fallback exists), and a missing runtime or image is an honest
 * fail-closed error. Staged source metadata is validated when present, and
 * the source ISO/data/save roots are resolved before returning NK_OK. */

NkResult nk_launch_prepare_session(
    NkLaunchSession *session,
    const NkGameEntry *game,
    const char *repo_or_install_root
);

/* Start the prepared session, launching the native runtime as a child process.
 * Immediately before spawn the session identity and the resolved executable
 * path are re-checked against each other: a session whose executable or title
 * identity was swapped after preparation is rejected instead of spawned. */
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
