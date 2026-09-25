/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NAKAGAWA_PACKAGE_BUILDER_H
#define NAKAGAWA_PACKAGE_BUILDER_H

#include "nk_types.h"
#include "nk_platform.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PACKAGE_BUILD_STAGE_IDLE = 0,
    PACKAGE_BUILD_STAGE_PREFLIGHT,
    PACKAGE_BUILD_STAGE_EXTRACT,
    PACKAGE_BUILD_STAGE_COMPILE,
    PACKAGE_BUILD_STAGE_PACKAGE,
    PACKAGE_BUILD_STAGE_COMPLETE,
    PACKAGE_BUILD_STAGE_FAILED,
    PACKAGE_BUILD_STAGE_CANCELLED
} PackageBuildStage;

typedef enum {
    PACKAGE_PROGRESS_STATUS_UNKNOWN = 0,
    PACKAGE_PROGRESS_STATUS_START,
    PACKAGE_PROGRESS_STATUS_RUNNING,
    PACKAGE_PROGRESS_STATUS_PASS,
    PACKAGE_PROGRESS_STATUS_FAIL
} PackageProgressStatus;

typedef struct {
    char stage[32];
    char status[32];
    char message[512];
    PackageBuildStage stage_enum;
    PackageProgressStatus status_enum;
} PackageProgressEvent;

#define PACKAGE_BUILD_MAX_OUTPUT_LINES 8
#define PACKAGE_BUILD_LINE_LEN 256

typedef struct {
    char disc_id[NK_MAX_DISC_ID_LEN];
    char title_name[NK_MAX_TITLE_LEN];
    PackageBuildStage current_stage;
    char current_stage_name[32];
    char current_message[512];
    char failure_boundary[512];
    char log_file_path[NK_MAX_PATH];
    char progress_file_path[NK_MAX_PATH];
    int64_t progress_file_offset;
    uint64_t start_time_ms;
    uint32_t elapsed_ms;
    bool is_building;
    bool is_complete;
    bool is_failed;
    bool is_cancelled;
    int exit_code;

    /* Circular buffer of last output lines */
    char output_lines[PACKAGE_BUILD_MAX_OUTPUT_LINES][PACKAGE_BUILD_LINE_LEN];
    int output_line_count;
    int output_line_head;

    /* Live child process tracking */
    NkProcessHandle process;
} PackageBuildSession;

/* Parse a single JSON progress line into a PackageProgressEvent.
 * The JSON object must contain "stage", "status", and "message" strings.
 * Returns true if valid, false on malformed or incomplete lines. */
bool package_builder_parse_progress_line(
    const char *line,
    size_t line_len,
    PackageProgressEvent *out_event
);

/* Initialize a build session for a title. */
void package_builder_init_session(
    PackageBuildSession *session,
    const char *disc_id,
    const char *title_name
);

/* Apply a progress event to the session state machine. */
void package_builder_apply_event(
    PackageBuildSession *session,
    const PackageProgressEvent *event
);

/* Append an output line to the circular buffer. */
void package_builder_add_output_line(
    PackageBuildSession *session,
    const char *line
);

/* Retrieve an output line by chronological index (0 = oldest, count - 1 = newest). */
const char *package_builder_get_output_line(
    const PackageBuildSession *session,
    int index
);

/* Locate Python 3 interpreter: checks PYTHON env, UCRT64 toolchain, and PATH. */
bool package_builder_find_python(char *out_path, size_t out_size);

/* Locate tools/nk_cli.py beside or one level above the player executable, then the working directory. */
bool package_builder_find_cli(const char *install_root, char *out_path, size_t out_size);

/* Start the package build as a child process using nk_platform_spawn_process. */
NkResult package_builder_start(
    PackageBuildSession *session,
    const char *python_path,
    const char *cli_path,
    const char *user_data_root,
    const char *log_dir
);

/* Poll for newly streamed lines and child process completion. */
void package_builder_poll(PackageBuildSession *session, uint64_t current_time_ms);

/* Terminate child process cleanly and transition to CANCELLED. */
void package_builder_cancel(PackageBuildSession *session);

#ifdef __cplusplus
}
#endif

#endif /* NAKAGAWA_PACKAGE_BUILDER_H */
