/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "package_builder.h"
#include "nk_json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

static inline void safe_str_copy(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0) return;
    if (!src) { dest[0] = '\0'; return; }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static const char *skip_whitespace(const char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    return s;
}

bool package_builder_parse_progress_line(
    const char *line,
    size_t line_len,
    PackageProgressEvent *out_event
) {
    if (!line || line_len == 0 || !out_event) return false;
    memset(out_event, 0, sizeof(*out_event));

    const char *start = skip_whitespace(line);
    if (*start != '{') return false;

    const char *end = line + line_len;
    while (end > start && isspace((unsigned char)*(end - 1))) {
        end--;
    }
    size_t trimmed_len = (size_t)(end - start);
    if (trimmed_len < 2 || start[trimmed_len - 1] != '}') return false;

    char err_buf[128];
    NkJsonNode *root = nk_json_parse(start, trimmed_len, err_buf, sizeof(err_buf));
    if (!root || !nk_json_is_object(root)) {
        if (root) nk_json_free(root);
        return false;
    }

    NkJsonNode *n_stage = nk_json_obj_get(root, "stage");
    NkJsonNode *n_status = nk_json_obj_get(root, "status");
    NkJsonNode *n_message = nk_json_obj_get(root, "message");

    if (!n_stage || !nk_json_is_string(n_stage) ||
        !n_status || !nk_json_is_string(n_status) ||
        !n_message || !nk_json_is_string(n_message)) {
        nk_json_free(root);
        return false;
    }

    safe_str_copy(out_event->stage, sizeof(out_event->stage), nk_json_get_string(n_stage));
    safe_str_copy(out_event->status, sizeof(out_event->status), nk_json_get_string(n_status));
    safe_str_copy(out_event->message, sizeof(out_event->message), nk_json_get_string(n_message));
    nk_json_free(root);

    /* Map stage string to enum */
    if (strcmp(out_event->stage, "preflight") == 0) {
        out_event->stage_enum = PACKAGE_BUILD_STAGE_PREFLIGHT;
    } else if (strcmp(out_event->stage, "extract") == 0) {
        out_event->stage_enum = PACKAGE_BUILD_STAGE_EXTRACT;
    } else if (strcmp(out_event->stage, "codegen") == 0 ||
               strcmp(out_event->stage, "compile") == 0) {
        out_event->stage_enum = PACKAGE_BUILD_STAGE_COMPILE;
    } else if (strcmp(out_event->stage, "package") == 0 ||
               strcmp(out_event->stage, "build_package") == 0) {
        out_event->stage_enum = PACKAGE_BUILD_STAGE_PACKAGE;
    } else {
        out_event->stage_enum = PACKAGE_BUILD_STAGE_IDLE;
    }

    /* Map status string to enum */
    if (strcmp(out_event->status, "START") == 0) {
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_START;
    } else if (strcmp(out_event->status, "RUNNING") == 0) {
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_RUNNING;
    } else if (strcmp(out_event->status, "PASS") == 0) {
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_PASS;
    } else if (strcmp(out_event->status, "FAIL") == 0) {
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_FAIL;
    } else {
        out_event->status_enum = PACKAGE_PROGRESS_STATUS_UNKNOWN;
    }

    return true;
}

void package_builder_init_session(
    PackageBuildSession *session,
    const char *disc_id,
    const char *title_name
) {
    if (!session) return;
    memset(session, 0, sizeof(*session));
    safe_str_copy(session->disc_id, sizeof(session->disc_id), disc_id);
    safe_str_copy(session->title_name, sizeof(session->title_name), title_name);
    session->current_stage = PACKAGE_BUILD_STAGE_IDLE;
    safe_str_copy(session->current_stage_name, sizeof(session->current_stage_name), "idle");
}

void package_builder_add_output_line(
    PackageBuildSession *session,
    const char *line
) {
    if (!session || !line) return;
    int slot = (session->output_line_head + session->output_line_count) % PACKAGE_BUILD_MAX_OUTPUT_LINES;
    if (session->output_line_count < PACKAGE_BUILD_MAX_OUTPUT_LINES) {
        session->output_line_count++;
    } else {
        session->output_line_head = (session->output_line_head + 1) % PACKAGE_BUILD_MAX_OUTPUT_LINES;
    }
    safe_str_copy(session->output_lines[slot], PACKAGE_BUILD_LINE_LEN, line);
}

const char *package_builder_get_output_line(
    const PackageBuildSession *session,
    int index
) {
    if (!session || index < 0 || index >= session->output_line_count) return "";
    int slot = (session->output_line_head + index) % PACKAGE_BUILD_MAX_OUTPUT_LINES;
    return session->output_lines[slot];
}

void package_builder_apply_event(
    PackageBuildSession *session,
    const PackageProgressEvent *event
) {
    if (!session || !event) return;

    if (event->stage[0]) {
        safe_str_copy(session->current_stage_name, sizeof(session->current_stage_name), event->stage);
        session->current_stage = event->stage_enum;
    }
    if (event->message[0]) {
        safe_str_copy(session->current_message, sizeof(session->current_message), event->message);
    }

    char line_buf[PACKAGE_BUILD_LINE_LEN];
    snprintf(line_buf, sizeof(line_buf), "[%.31s] %.31s: %.180s",
             event->stage, event->status, event->message);
    package_builder_add_output_line(session, line_buf);

    if (event->status_enum == PACKAGE_PROGRESS_STATUS_FAIL) {
        session->current_stage = PACKAGE_BUILD_STAGE_FAILED;
        session->is_failed = true;
        safe_str_copy(session->failure_boundary, sizeof(session->failure_boundary), event->message);
    } else if (event->stage_enum == PACKAGE_BUILD_STAGE_PACKAGE &&
               event->status_enum == PACKAGE_PROGRESS_STATUS_PASS) {
        session->current_stage = PACKAGE_BUILD_STAGE_COMPLETE;
        session->is_complete = true;
    }
}

/* Scan PATH for the first of names that exists as a file. Shared by the
 * interpreter and toolchain lookups: both must see exactly the PATH a spawned
 * build child would see, with no hidden fallback location. */
static bool scan_path_for_names(const char *const *names, size_t name_count,
                                char *out_path, size_t out_size) {
    const char *path_env = getenv("PATH");
    if (!path_env || !path_env[0]) return false;

    char path_copy[32768];
    safe_str_copy(path_copy, sizeof(path_copy), path_env);

#if defined(_WIN32) || defined(_WIN64)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    char *dir = path_copy;
    while (dir) {
        char *next = strchr(dir, sep);
        if (next) *next++ = '\0';
        /* A PATH entry too long for a candidate path cannot name a usable tool. */
        if (dir[0] && strlen(dir) < NK_MAX_PATH) {
            for (size_t i = 0; i < name_count; i++) {
                char candidate[NK_MAX_PATH * 2];
                snprintf(candidate, sizeof(candidate), "%.*s%c%s", NK_MAX_PATH - 1, dir,
                         nk_platform_path_separator(), names[i]);
                if (nk_platform_file_exists(candidate)) {
                    if (nk_platform_absolute_path(candidate, out_path, out_size)) {
                        return true;
                    }
                    safe_str_copy(out_path, out_size, candidate);
                    return true;
                }
            }
        }
        dir = next;
    }
    return false;
}

bool package_builder_find_python(char *out_path, size_t out_size) {
    if (!out_path || out_size == 0) return false;
    out_path[0] = '\0';

    /* 1. Check PYTHON environment variable */
    const char *env_py = getenv("PYTHON");
    if (env_py && env_py[0] && nk_platform_file_exists(env_py)) {
        if (nk_platform_absolute_path(env_py, out_path, out_size)) {
            return true;
        }
        safe_str_copy(out_path, out_size, env_py);
        return true;
    }

#if defined(_WIN32) || defined(_WIN64)
    /* 2. Check standard MSYS2 UCRT64 toolchain locations */
    static const char *kWinCandidates[] = {
        "C:\\msys64\\ucrt64\\bin\\python3.exe",
        "C:\\msys64\\ucrt64\\bin\\python.exe",
        "C:/msys64/ucrt64/bin/python3.exe",
        "C:/msys64/ucrt64/bin/python.exe"
    };
    for (size_t i = 0; i < sizeof(kWinCandidates) / sizeof(kWinCandidates[0]); i++) {
        if (nk_platform_file_exists(kWinCandidates[i])) {
            safe_str_copy(out_path, out_size, kWinCandidates[i]);
            return true;
        }
    }
#endif

    /* 3. Scan PATH for python3 / python */
#if defined(_WIN32) || defined(_WIN64)
    static const char *kPathNames[] = { "python3.exe", "python.exe" };
#else
    static const char *kPathNames[] = { "python3", "python" };
#endif
    if (scan_path_for_names(kPathNames, sizeof(kPathNames) / sizeof(kPathNames[0]),
                            out_path, out_size)) {
        return true;
    }

#if !defined(_WIN32) && !defined(_WIN64)
    static const char *kPosixCandidates[] = {
        "/usr/bin/python3",
        "/usr/local/bin/python3",
        "/usr/bin/python"
    };
    for (size_t i = 0; i < sizeof(kPosixCandidates) / sizeof(kPosixCandidates[0]); i++) {
        if (nk_platform_file_exists(kPosixCandidates[i])) {
            safe_str_copy(out_path, out_size, kPosixCandidates[i]);
            return true;
        }
    }
#endif

    return false;
}

/* Append "<root>/<rel>" unless root is empty or the slots are full. */
static void cli_add_rooted(char out[][NK_MAX_PATH], int *count, int max_out,
                           const char *root, const char *rel) {
    if (!root || !root[0] || *count >= max_out) return;
    size_t len = strlen(root);
    bool has_sep = root[len - 1] == '/' || root[len - 1] == '\\';
    snprintf(out[*count], NK_MAX_PATH, "%s%s%s", root, has_sep ? "" : "/", rel);
    (*count)++;
}

int package_builder_cli_candidate_paths(
    const char *install_root,
    const char *override_root,
    char out[][NK_MAX_PATH],
    int max_out
) {
    if (!out || max_out <= 0) return 0;
    int count = 0;

    /* 1. Explicit override wins (NK_INSTALL_ROOT: the folder containing tools/). */
    cli_add_rooted(out, &count, max_out, override_root, "tools/nk_cli.py");
    /* 2. The executable's own folder: a checkout runs as <checkout>/build/, a
     *    packaged player may carry tools/ beside nakagawa_player.exe. */
    cli_add_rooted(out, &count, max_out, install_root, "tools/nk_cli.py");
    /* 3. One level above the executable: <checkout>/build/../tools. */
    cli_add_rooted(out, &count, max_out, install_root, "../tools/nk_cli.py");
    /* 4. The documented v0.0.1 release layout: bin/ beside source/, and the
     *    public source export carries tools/nk_cli.py. */
    cli_add_rooted(out, &count, max_out, install_root, "../source/tools/nk_cli.py");
    /* 5-6. The working directory and its parent, unchanged from before. */
    static const char *kRelatives[] = { "tools/nk_cli.py", "../tools/nk_cli.py" };
    for (size_t i = 0; i < sizeof(kRelatives) / sizeof(kRelatives[0]) && count < max_out; i++) {
        snprintf(out[count], NK_MAX_PATH, "%s", kRelatives[i]);
        count++;
    }
    return count;
}

bool package_builder_find_cli(const char *install_root, char *out_path, size_t out_size) {
    if (!out_path || out_size == 0) return false;
    out_path[0] = '\0';

    const char *override_root = getenv("NK_INSTALL_ROOT");
    char candidates[PACKAGE_BUILDER_CLI_MAX_CANDIDATES][NK_MAX_PATH];
    int count = package_builder_cli_candidate_paths(
        install_root, override_root, candidates, PACKAGE_BUILDER_CLI_MAX_CANDIDATES);
    for (int i = 0; i < count; i++) {
        if (nk_platform_file_exists(candidates[i])) {
            if (nk_platform_absolute_path(candidates[i], out_path, out_size)) return true;
            safe_str_copy(out_path, out_size, candidates[i]);
            return true;
        }
    }
    return false;
}

void package_builder_describe_cli_not_found(const char *install_root,
                                            char *out, size_t out_size) {
    if (!out || out_size == 0) return;
    /* Trim a trailing separator so the shown locations read cleanly. */
    char root[NK_MAX_PATH];
    if (install_root && install_root[0]) {
        safe_str_copy(root, sizeof(root), install_root);
        size_t len = strlen(root);
        while (len > 1 && (root[len - 1] == '/' || root[len - 1] == '\\')) {
            root[--len] = '\0';
        }
    } else {
        safe_str_copy(root, sizeof(root), "the player folder");
    }
    snprintf(out, out_size,
             "tools/nk_cli.py was not found. Searched, in order: the NK_INSTALL_ROOT "
             "environment variable, %s/tools, %s/../tools, %s/../source/tools (the "
             "release layout), the working directory and its parent. Fix: run the player "
             "from a Nakagawa Recomp source checkout, keep a tools folder beside "
             "nakagawa_player.exe, or set NK_INSTALL_ROOT to the folder that contains "
             "tools/ (for example the release's source folder).",
             root, root, root);
}

bool package_builder_find_tool(const char *name, char *out_path, size_t out_size) {
    if (!name || !name[0] || !out_path || out_size == 0) return false;
    out_path[0] = '\0';

#if defined(_WIN32) || defined(_WIN64)
    char exe_name[NK_MAX_PATH + 8];
    const char *names[2];
    size_t name_count;
    if (strlen(name) > 4 && strcmp(name + strlen(name) - 4, ".exe") == 0) {
        names[0] = name;
        name_count = 1;
    } else {
        snprintf(exe_name, sizeof(exe_name), "%s.exe", name);
        names[0] = exe_name;
        names[1] = name;
        name_count = 2;
    }
#else
    const char *names[1] = { name };
    size_t name_count = 1;
#endif
    return scan_path_for_names(names, name_count, out_path, out_size);
}

bool package_builder_toolchain_missing(
    bool have_python, bool have_gcc, bool have_make,
    char *out_tool, size_t out_tool_size,
    char *out_message, size_t out_message_size
) {
    const char *tool = NULL;
    const char *fix = NULL;
    if (!have_python) {
        tool = "python";
        fix = "Install CPython 3.14 (see docs/SETUP.md) or set the PYTHON environment "
              "variable to the interpreter.";
    } else if (!have_gcc) {
        tool = "gcc";
        fix = "Install the MSYS2 UCRT64 toolchain (mingw-w64-ucrt-x86_64-gcc) and put "
              "its bin directory on PATH.";
    } else if (!have_make) {
        tool = "mingw32-make";
        fix = "Install the MSYS2 UCRT64 toolchain (mingw-w64-ucrt-x86_64-make) and put "
              "its bin directory on PATH.";
    }
    if (!tool) {
        if (out_tool && out_tool_size) out_tool[0] = '\0';
        if (out_message && out_message_size) out_message[0] = '\0';
        return false;
    }
    if (out_tool && out_tool_size) snprintf(out_tool, out_tool_size, "%s", tool);
    if (out_message && out_message_size) {
        snprintf(out_message, out_message_size,
                 "BUILD_TOOLCHAIN_MISSING: %s was not found on PATH. The package build "
                 "runs python, gcc and mingw32-make, so it cannot start. %s "
                 "Automatic build-prerequisite installation is in the works (#324).",
                 tool, fix);
    }
    return true;
}

NkResult package_builder_start(
    PackageBuildSession *session,
    const char *python_path,
    const char *cli_path,
    const char *user_data_root,
    const char *log_dir
) {
    if (!session || !python_path || !cli_path || !session->disc_id[0]) {
        return NK_ERROR_GENERIC;
    }

    /* Set up progress file and log file paths */
    const char *ldir = (log_dir && log_dir[0]) ? log_dir : ".";
    nk_platform_mkdir_p(ldir);

    snprintf(session->progress_file_path, sizeof(session->progress_file_path),
             "%s%cbuild_%s_progress.jsonl", ldir, nk_platform_path_separator(), session->disc_id);
    snprintf(session->log_file_path, sizeof(session->log_file_path),
             "%s%cbuild_%s.log", ldir, nk_platform_path_separator(), session->disc_id);

    /* Remove previous files if present */
    remove(session->progress_file_path);
    remove(session->log_file_path);
    session->progress_file_offset = 0;

    const char *argv[16];
    int argc = 0;
    argv[argc++] = python_path;
    argv[argc++] = cli_path;
    argv[argc++] = "build-package";
    argv[argc++] = session->disc_id;
    argv[argc++] = "--progress-json";
    argv[argc++] = session->progress_file_path;
    argv[argc++] = "--log-file";
    argv[argc++] = session->log_file_path;

    if (user_data_root && user_data_root[0]) {
        argv[argc++] = "--user-data-root";
        argv[argc++] = user_data_root;
    }
    argv[argc] = NULL;

    bool spawned = nk_platform_spawn_process(
        python_path,
        argv,
        NULL,
        NULL,
        &session->process
    );

    if (!spawned) {
        session->is_building = false;
        session->is_failed = true;
        session->current_stage = PACKAGE_BUILD_STAGE_FAILED;
        safe_str_copy(session->failure_boundary, sizeof(session->failure_boundary),
                      "Failed to spawn background build process.");
        return NK_ERROR_PROCESS_SPAWN;
    }

    session->is_building = true;
    session->is_complete = false;
    session->is_failed = false;
    session->is_cancelled = false;
    session->exit_code = -1;
    session->current_stage = PACKAGE_BUILD_STAGE_PREFLIGHT;
    safe_str_copy(session->current_stage_name, sizeof(session->current_stage_name), "preflight");
    safe_str_copy(session->current_message, sizeof(session->current_message), "Starting package build...");

    char spawn_msg[PACKAGE_BUILD_LINE_LEN];
    snprintf(spawn_msg, sizeof(spawn_msg), "[build] START: Building package for %s", session->disc_id);
    package_builder_add_output_line(session, spawn_msg);

    return NK_OK;
}

static void read_new_progress_lines(PackageBuildSession *session) {
    if (!session || !session->progress_file_path[0]) return;
    FILE *f = fopen(session->progress_file_path, "rb");
    if (!f) return;

    if (session->progress_file_offset > 0) {
        if (fseek(f, (long)session->progress_file_offset, SEEK_SET) != 0) {
            fclose(f);
            return;
        }
    }

    char line_buf[1024];
    while (fgets(line_buf, sizeof(line_buf), f)) {
        size_t len = strlen(line_buf);
        if (len > 0 && line_buf[len - 1] == '\n') {
            session->progress_file_offset = ftell(f);
            PackageProgressEvent ev;
            if (package_builder_parse_progress_line(line_buf, len, &ev)) {
                package_builder_apply_event(session, &ev);
            }
        } else {
            /* Incomplete line: rewind to before this partial read */
            fseek(f, (long)session->progress_file_offset, SEEK_SET);
            break;
        }
    }
    fclose(f);
}

void package_builder_poll(PackageBuildSession *session, uint64_t current_time_ms) {
    if (!session || !session->is_building) return;

    if (session->start_time_ms > 0 && current_time_ms >= session->start_time_ms) {
        session->elapsed_ms = (uint32_t)(current_time_ms - session->start_time_ms);
    }

    read_new_progress_lines(session);

    if (!session->process.is_active) {
        return;
    }

    bool running = nk_platform_is_process_running(&session->process);
    if (!running) {
        session->exit_code = nk_platform_wait_process(&session->process, 0);
        session->is_building = false;
        nk_platform_close_process(&session->process);

        /* Read any remaining flushed lines */
        read_new_progress_lines(session);

        if (session->exit_code != 0) {
            session->is_failed = true;
            if (session->current_stage != PACKAGE_BUILD_STAGE_FAILED) {
                session->current_stage = PACKAGE_BUILD_STAGE_FAILED;
            }
            if (!session->failure_boundary[0]) {
                /* If no boundary message was recorded from JSON, extract last error from log */
                if (session->log_file_path[0]) {
                    FILE *lf = fopen(session->log_file_path, "rb");
                    if (lf) {
                        char last_line[512] = "";
                        char cur_line[512];
                        while (fgets(cur_line, sizeof(cur_line), lf)) {
                            size_t l = strlen(cur_line);
                            while (l > 0 && (cur_line[l - 1] == '\r' || cur_line[l - 1] == '\n')) {
                                cur_line[--l] = '\0';
                            }
                            if (l > 0) safe_str_copy(last_line, sizeof(last_line), cur_line);
                        }
                        fclose(lf);
                        if (last_line[0]) {
                            safe_str_copy(session->failure_boundary, sizeof(session->failure_boundary), last_line);
                        }
                    }
                }
                if (!session->failure_boundary[0]) {
                    snprintf(session->failure_boundary, sizeof(session->failure_boundary),
                             "Build child process failed with exit code %d.", session->exit_code);
                }
            }
        }
    }
}

void package_builder_cancel(PackageBuildSession *session) {
    if (!session) return;
    if (session->is_building) {
        nk_platform_terminate_process(&session->process);
        nk_platform_close_process(&session->process);
        session->is_building = false;
        session->is_cancelled = true;
        session->current_stage = PACKAGE_BUILD_STAGE_CANCELLED;
        safe_str_copy(session->current_stage_name, sizeof(session->current_stage_name), "cancelled");
        safe_str_copy(session->current_message, sizeof(session->current_message), "Build cancelled by user.");
        package_builder_add_output_line(session, "[build] CANCEL: Process terminated by user");
    }
}
