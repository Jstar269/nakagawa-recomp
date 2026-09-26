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
    char item_id[96];
    char error_code[48];
    uint64_t item_received_bytes;
    uint64_t item_total_bytes;
    uint64_t total_received_bytes;
    uint64_t total_bytes;
    bool install_complete;
    PackageBuildStage stage_enum;
    PackageProgressStatus status_enum;
} PackageProgressEvent;

#define PACKAGE_BUILDER_MAX_PREREQUISITES 32
typedef struct {
    char id[64];
    char name[128];
    char version[48];
    char host[128];
    char allowed_hosts[256];
    char url[512];
    char sha256[65];
    char filename[256];
    char license[192];
    char notice_path[NK_MAX_PATH];
    uint64_t size_bytes;
    bool installed;
} PackagePrerequisite;

typedef struct {
    PackagePrerequisite items[PACKAGE_BUILDER_MAX_PREREQUISITES];
    size_t count;
    uint64_t total_bytes;
} PackagePrerequisiteList;

#define PACKAGE_BUILD_MAX_OUTPUT_LINES 8
#define PACKAGE_BUILD_LINE_LEN 256

typedef struct {
    char disc_id[NK_MAX_DISC_ID_LEN];
    char title_name[NK_MAX_TITLE_LEN];
    PackageBuildStage current_stage;
    char current_stage_name[32];
    char current_message[512];
    char failure_boundary[512];
    char current_item_id[96];
    char failure_code[48];
    uint64_t item_received_bytes;
    uint64_t item_total_bytes;
    uint64_t total_received_bytes;
    uint64_t total_bytes;
    bool prerequisite_install_complete;
    char log_file_path[NK_MAX_PATH];
    char progress_file_path[NK_MAX_PATH];
    char cancel_file_path[NK_MAX_PATH];
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
bool package_builder_find_python_in_root(const char *data_root,
                                        char *out_path, size_t out_size);

/* Maximum ordered tools/nk_cli.py candidates from
 * package_builder_cli_candidate_paths(). */
#define PACKAGE_BUILDER_CLI_MAX_CANDIDATES 6

/* Pure: fill out[0..return-1] with the ordered tools/nk_cli.py candidate paths.
 * override_root is the NK_INSTALL_ROOT value (NULL/empty when unset) and wins;
 * install_root is the executable's folder (NULL/empty when unknown); the working
 * directory and its parent are appended as relative paths. */
int package_builder_cli_candidate_paths(
    const char *install_root,
    const char *override_root,
    char out[][NK_MAX_PATH],
    int max_out
);

/* Locate tools/nk_cli.py: NK_INSTALL_ROOT, beside/above the executable, the
 * v0.0.1 release layout (<exe>/../source/tools), then the working directory. */
bool package_builder_find_cli(const char *install_root, char *out_path, size_t out_size);

/* Pure: guidance text for the CLI_NOT_FOUND card, naming every location the
 * search covers and the NK_INSTALL_ROOT fix. Fits in PlayerLastError.message. */
void package_builder_describe_cli_not_found(
    const char *install_root, char *out, size_t out_size
);

/* Locate a host build tool ("gcc", "mingw32-make") on PATH only: the spawned
 * build child sees exactly this PATH, so no hidden fallback location counts. */
bool package_builder_find_tool(const char *name, char *out_path, size_t out_size);
bool package_builder_find_tool_in_root(const char *name, const char *data_root,
                                       char *out_path, size_t out_size);

/* Read the pinned prerequisites from assets/prereq_manifest.json adjacent to
 * the source tree containing cli_path. Only ready artifacts with an HTTPS URL,
 * a non-empty host allow-list, size, and digest are displayed. */
bool package_builder_load_prerequisites(const char *cli_path,
                                        PackagePrerequisiteList *out_list,
                                        char *error, size_t error_size);
void package_builder_mark_prerequisites_installed(PackagePrerequisiteList *list,
                                                   const char *data_root);

typedef struct {
    const char *final_url;
    bool has_content_length;
    uint64_t content_length;
    void *context;
    bool (*read)(void *context, unsigned char *buffer, size_t capacity,
                 size_t *bytes_read);
    void (*close)(void *context);
} PackageHttpResponse;

typedef bool (*PackageHttpOpen)(void *context, const char *url,
                                const char *allowed_hosts,
                                PackageHttpResponse *response);
typedef bool (*PackageDownloadContinue)(void *context, uint64_t received,
                                        uint64_t expected);

/* Download one manifest item to a .part file, check exact byte count and SHA-256,
 * then atomically promote it. A NULL transport selects native WinHTTP. */
bool package_builder_download_verified(
    const PackagePrerequisite *item, const char *destination,
    PackageHttpOpen transport, void *transport_context,
    PackageDownloadContinue progress, void *progress_context,
    char *error_code, size_t error_code_size,
    char *error_message, size_t error_message_size);

typedef struct {
    void *thread_handle;
    volatile int64_t received_bytes;
    volatile int done;
    volatile int cancel_requested;
    bool succeeded;
    char error_code[48];
    char error_message[512];
    char archive_path[NK_MAX_PATH];
    char python_path[NK_MAX_PATH];
    char prerequisites_root[NK_MAX_PATH];
    char staging_path[NK_MAX_PATH];
    PackagePrerequisite item;
} PackageBootstrapSession;

bool package_builder_bootstrap_python_start(
    PackageBootstrapSession *session, const PackagePrerequisite *python_item,
    const char *data_root);
bool package_builder_bootstrap_python_poll(PackageBootstrapSession *session,
                                          uint64_t *received_bytes,
                                          bool *finished, bool *succeeded,
                                          char *error_code, size_t error_code_size,
                                          char *error_message, size_t error_message_size);
void package_builder_bootstrap_python_cancel(PackageBootstrapSession *session);
void package_builder_bootstrap_python_close(PackageBootstrapSession *session);

NkResult package_builder_start_prerequisite_fetch(
    PackageBuildSession *session, const char *python_path, const char *cli_path,
    const char *data_root, const char *log_dir, bool bootstrapped_python);
void package_builder_request_prerequisite_cancel(
    PackageBuildSession *session, const char *cancel_file_path);
bool package_builder_remove_downloaded_tools(const char *data_root,
                                            char *error_code, size_t error_code_size,
                                            char *error_message, size_t error_message_size);

/* Pure: report the first missing build tool (python, gcc, mingw32-make).
 * Returns false and clears the outputs when all three are present. On success
 * out_tool holds the tool name and out_message a BUILD_TOOLCHAIN_MISSING card
 * body that names it and how to install it. */
bool package_builder_toolchain_missing(
    bool have_python, bool have_gcc, bool have_make,
    char *out_tool, size_t out_tool_size,
    char *out_message, size_t out_message_size
);

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
