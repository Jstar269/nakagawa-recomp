/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_launch.h"
#include "generated/nk_title_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline void safe_copy_path(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0) return;
    if (!src) { dest[0] = '\0'; return; }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

/* Confirm a directory exists and can actually be written to.
 *
 * nk_platform_dir_exists answers a different question: a directory under
 * Program Files or /usr/lib exists and is still unwritable for the user the
 * runtime runs as. Save data that silently fails to persist is worse than a
 * launch that reports it has nowhere to write, so this probes for real. */
static bool ensure_writable_dir(const char *path) {
    if (!path || !*path) return false;
    if (!nk_platform_dir_exists(path) && !nk_platform_mkdir_p(path)) return false;

    char probe[NK_MAX_PATH * 2];
    int w = snprintf(probe, sizeof(probe), "%s%c.nk_write_probe", path, nk_platform_path_separator());
    if (w <= 0 || (size_t)w >= sizeof(probe)) return false;

    FILE *f = fopen(probe, "wb");
    if (!f) return false;
    fclose(f);
    remove(probe);
    return true;
}

/* Helper to check candidate binary paths */
static bool find_candidate_executable(
    const char *root,
    const char *title_id,
    char *out_path,
    size_t max_len
) {
    /* Candidate 0: Direct executable path passed as root */
    if (root && nk_platform_file_exists(root) && !nk_platform_dir_exists(root)) {
        snprintf(out_path, max_len, "%s", root);
        return true;
    }

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

bool nk_launch_runtime_available(const char *root, const char *title_id) {
    char resolved[NK_MAX_PATH];
    const char *effective_root = (root && *root) ? root : ".";
    return find_candidate_executable(effective_root, title_id, resolved, sizeof(resolved));
}

static bool find_candidate_image(
    const char *working_dir,
    const char *executable_path,
    const char *title_id,
    char *out_path,
    size_t max_len
) {
    char cand[NK_MAX_PATH * 2];
    char sep = nk_platform_path_separator();

    /* 1. Alongside executable: <executable without extension>_image.bin
     *
     * This used to run only when the name ended in .exe, so a normal
     * extensionless Linux or macOS binary such as /opt/nakagawa/bin/my-title
     * never probed /opt/nakagawa/bin/my-title_image.bin. For a non-HST title
     * the later hard-coded probes miss it too, so the runtime was started with
     * no --image at all and src/rt/driver.c exited through its
     * insufficient-arguments path.
     *
     * Only a dot in the FINAL path component can be an extension:
     * /opt/nakagawa.d/bin/my-title has a dot but no extension, and a leading
     * dot (.hidden) names the file rather than separating an extension. */
    if (executable_path && *executable_path) {
        snprintf(cand, sizeof(cand), "%s", executable_path);
        char *base = strrchr(cand, '/');
        char *base_alt = strrchr(cand, sep);
        if (base_alt && (!base || base_alt > base)) base = base_alt;
        base = base ? base + 1 : cand;

        char *ext = strrchr(base, '.');
        char *suffix_at = NULL;
        if (ext && ext != base) {
            /* Windows executables carry .exe; replace whatever extension the
             * host uses so the sibling name matches the build's convention. */
            suffix_at = ext;
        } else {
            /* No extension: append directly. */
            suffix_at = cand + strlen(cand);
        }
        size_t used = (size_t)(suffix_at - cand);
        if (used + sizeof("_image.bin") <= sizeof(cand)) {
            snprintf(suffix_at, sizeof(cand) - used, "_image.bin");
            if (nk_platform_file_exists(cand)) {
                snprintf(out_path, max_len, "%s", cand);
                return true;
            }
        }
        /* Or <dir>/hst_image.bin */
        char dir[NK_MAX_PATH];
        snprintf(dir, sizeof(dir), "%s", executable_path);
        char *last_slash = strrchr(dir, '/');
        if (!last_slash) last_slash = strrchr(dir, '\\');
        if (last_slash) {
            *last_slash = '\0';
            snprintf(cand, sizeof(cand), "%s%chst_image.bin", dir, sep);
            if (nk_platform_file_exists(cand)) {
                snprintf(out_path, max_len, "%s", cand);
                return true;
            }
        }
    }

    /* 2. <working_dir>/build/hst/hst_image.bin */
    snprintf(cand, sizeof(cand), "%s%cbuild%chst%chst_image.bin", working_dir, sep, sep, sep);
    if (nk_platform_file_exists(cand)) {
        snprintf(out_path, max_len, "%s", cand);
        return true;
    }

    /* 3. <working_dir>/runtime/hst_image.bin */
    snprintf(cand, sizeof(cand), "%s%cruntime%chst_image.bin", working_dir, sep, sep);
    if (nk_platform_file_exists(cand)) {
        snprintf(out_path, max_len, "%s", cand);
        return true;
    }

    /* 4. <working_dir>/build/<title_id>/<title_id>_image.bin */
    if (title_id && *title_id) {
        snprintf(cand, sizeof(cand), "%s%cbuild%c%s%c%s_image.bin", working_dir, sep, sep, title_id, sep, title_id);
        if (nk_platform_file_exists(cand)) {
            snprintf(out_path, max_len, "%s", cand);
            return true;
        }
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
    if (nk_platform_file_exists(root) && !nk_platform_dir_exists(root)) {
        /* If root is a file, derive working directory from its parent */
        char *last_sep = strrchr(session->working_directory, '/');
        if (!last_sep) last_sep = strrchr(session->working_directory, '\\');
        if (last_sep) {
            *last_sep = '\0';
            /* If the parent is build/<title>, move up to the repository root so
               assets are found.
               Only an EXACT "build" component counts. strstr matched any
               component merely beginning with those five letters, so a directly
               supplied executable under a path such as /opt/build-tools/runtime
               truncated the working directory to /opt, and the runtime then
               resolved its data root, fonts, VFPU tables and filesystem from the
               wrong place. Scan for the last exact match so the deepest
               build/<title> layout wins. */
            char wd_sep = nk_platform_path_separator();
            char *scan = session->working_directory;
            char *build_at = NULL;
            for (;;) {
                char *hit = strstr(scan, "build");
                if (!hit) break;
                char after = hit[5];
                bool starts_component = (hit == session->working_directory)
                                     || hit[-1] == '/' || hit[-1] == wd_sep;
                bool ends_component = (after == '\0') || after == '/' || after == wd_sep;
                if (starts_component && ends_component) {
                    build_at = hit;
                }
                scan = hit + 5;
            }
            if (build_at && build_at > session->working_directory) {
                build_at[-1] = '\0';
            }
        }
    }
    snprintf(session->title_id, sizeof(session->title_id), "%s", game->title_id);
    snprintf(session->disc_id, sizeof(session->disc_id), "%s", game->disc_id);
    snprintf(session->prepared_root, sizeof(session->prepared_root), "%s", game->prepared_root);

    session->config.resolution_scale = 1;
    session->config.fps_cap = 60;
    session->config.fullscreen = false;
    session->config.vsync = true;
    session->config.benchmark_mode = false;
    session->config.diagnostic_mode = false;
    session->config.gui_mode = false;

    /* 1. Resolve executable */
    if (!find_candidate_executable(root, game->title_id, session->executable_path, sizeof(session->executable_path))) {
        snprintf(session->last_error, sizeof(session->last_error), "Runtime binary not found under root: %s", root);
        return NK_ERROR_FILE_NOT_FOUND;
    }

    /* 2. Resolve image.bin */
    find_candidate_image(session->working_directory, session->executable_path, game->title_id, session->image_path, sizeof(session->image_path));

    /* 3. Resolve title catalog entry & addresses */
    const NkTitleEntry *entry = nk_title_catalog_find_by_disc_id(session->disc_id);
    if (!entry) entry = nk_title_catalog_find_by_id(session->title_id);
    /* No catalog entry means no known load address. The previous fallback started
       the runtime at a hard-coded 0x0029a060 with a zero base -- an address that
       belongs to no title in this tree, so the guest was loaded at 0 and executed
       from a constant, which is a fabricated launch rather than a refusal. A title
       the catalog does not describe is exactly the fail-closed case. */
    if (!entry || !entry->executable_entry) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "No catalog entry describes this title (disc_id=%.16s title_id=%.32s)",
                 session->disc_id, session->title_id);
        return NK_ERROR_UNSUPPORTED_TITLE;
    }
    session->base_address = entry->executable_base;
    session->entry_point = entry->executable_entry;

    /* 4. Resolve data root */
    char sep = nk_platform_path_separator();
    if (entry && entry->data_root) {
        char cand_data[NK_MAX_PATH * 2];
        int w = snprintf(cand_data, sizeof(cand_data), "%s%c%s", session->working_directory, sep, entry->data_root);
        if (w > 0 && (size_t)w < sizeof(cand_data) && nk_platform_dir_exists(cand_data)) {
            /* Catalog data_root values are relative to the repository/install
               root, but SR_DATAROOT is deliberately fail-closed when relative.
               Resolve the path before handing it to the runtime. */
            char absolute[NK_MAX_PATH];
            if (nk_platform_absolute_path(cand_data, absolute, sizeof(absolute))) {
                safe_copy_path(session->dataroot_path, sizeof(session->dataroot_path), absolute);
            } else {
                safe_copy_path(session->dataroot_path, sizeof(session->dataroot_path), cand_data);
            }
        }
    }
    if (session->dataroot_path[0] == '\0') {
        /* Check alongside ISO directory: EXTRACTED/PSP_GAME/USRDIR/xbdata_extracted */
        char iso_dir[NK_MAX_PATH];
        safe_copy_path(iso_dir, sizeof(iso_dir), game->iso_path);
        char *s = strstr(iso_dir, "place_game_here");
        if (s) {
            s[15] = '\0'; /* truncate to place_game_here */
            char cand_data[NK_MAX_PATH * 2];
            int w = snprintf(cand_data, sizeof(cand_data), "%s%cEXTRACTED%cPSP_GAME%cUSRDIR%cxbdata_extracted",
                             iso_dir, sep, sep, sep, sep);
            if (w > 0 && (size_t)w < sizeof(cand_data) && nk_platform_dir_exists(cand_data)) {
                safe_copy_path(session->dataroot_path, sizeof(session->dataroot_path), cand_data);
            }
        }
    }

    /* 5. Resolve font directory */
    char cand_font[NK_MAX_PATH * 2];
    int fw = snprintf(cand_font, sizeof(cand_font), "%s%cfont", session->working_directory, sep);
    if (fw > 0 && (size_t)fw < sizeof(cand_font) && nk_platform_dir_exists(cand_font)) {
        safe_copy_path(session->font_dir, sizeof(session->font_dir), cand_font);
    }

    /* 5b. Resolve a writable Memory Stick root.
     *
     * SR_MEMSTICK was hard-coded to the relative path "saves", which the
     * runtime resolves under its own working directory. A packaged install
     * below a read-only location -- Program Files, /usr/lib, a signed app
     * bundle -- therefore could not create save data at all, and every title
     * was routed into the same unintended root. Prefer the title catalog's own
     * memory_stick_root when that location is genuinely writable, which keeps
     * the documented per-title developer layout working from a repository
     * checkout; otherwise use the platform per-user save directory with a
     * per-disc subdirectory, which is writable by construction and keeps
     * titles apart. */
    session->memstick_root[0] = 0;
    if (entry && entry->memory_stick_root && *entry->memory_stick_root) {
        char cand_ms[NK_MAX_PATH * 2];
        int w = snprintf(cand_ms, sizeof(cand_ms), "%s%c%s",
                         session->working_directory, sep, entry->memory_stick_root);
        if (w > 0 && (size_t)w < sizeof(session->memstick_root) && ensure_writable_dir(cand_ms)) {
            safe_copy_path(session->memstick_root, sizeof(session->memstick_root), cand_ms);
        }
    }
    if (session->memstick_root[0] == 0) {
        char saves_root[NK_MAX_PATH];
        if (nk_platform_get_path(NK_PATH_SAVES, saves_root, sizeof(saves_root))) {
            const char *slot = session->disc_id[0] ? session->disc_id
                             : (session->title_id[0] ? session->title_id : "unidentified");
            char cand_ms[NK_MAX_PATH * 2];
            int w = snprintf(cand_ms, sizeof(cand_ms), "%s%c%s", saves_root, sep, slot);
            if (w > 0 && (size_t)w < sizeof(session->memstick_root) && ensure_writable_dir(cand_ms)) {
                safe_copy_path(session->memstick_root, sizeof(session->memstick_root), cand_ms);
            }
        }
    }
    if (session->memstick_root[0] == 0) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "No writable save location: neither the install directory nor the "
                 "per-user save directory could be created or written.");
        return NK_ERROR_IO;
    }

    /* 6. Resolve ISO path */
    if (nk_platform_file_exists(game->iso_path)) {
        snprintf(session->iso_path, sizeof(session->iso_path), "%s", game->iso_path);
    } else {
        /* Check fallback under prepared_root/disc/game.iso */
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
    char env_dataroot[NK_MAX_PATH + 16];
    char env_font[NK_MAX_PATH + 16];
    char env_fs[32];
    char env_memstick[NK_MAX_PATH + 16];
    char env_tables[64];
    char env_boot_event[NK_MAX_PATH + 32];
    char sep = nk_platform_path_separator();

    snprintf(env_iso, sizeof(env_iso), "PSP_ISO=%s", session->iso_path);
    snprintf(env_fps, sizeof(env_fps), "SR_FPS_CAP=%d", session->config.fps_cap);
    snprintf(env_ge, sizeof(env_ge), "SR_GPU_GE=1");
    snprintf(env_scale, sizeof(env_scale), "SR_RESOLUTION_SCALE=%d", session->config.resolution_scale);
    snprintf(env_vsync, sizeof(env_vsync), "SR_VSYNC=%d", session->config.vsync ? 1 : 0);
    snprintf(env_fs, sizeof(env_fs), "SR_FSDIR=fs");
    snprintf(env_memstick, sizeof(env_memstick), "SR_MEMSTICK=%s", session->memstick_root);
    snprintf(env_tables, sizeof(env_tables), "PSP_VFPU_TABLES=assets%cvfpu", sep);

    const char *envp[24];
    int env_count = 0;
    envp[env_count++] = env_iso;
    envp[env_count++] = env_fps;
    envp[env_count++] = env_ge;
    envp[env_count++] = env_scale;
    envp[env_count++] = env_vsync;
    envp[env_count++] = env_fs;
    /* A session assembled by hand rather than by nk_launch_prepare_session may
       carry no resolved root; leaving SR_MEMSTICK unset lets the runtime apply
       its own default instead of being pointed at an empty path. */
    if (session->memstick_root[0]) {
        envp[env_count++] = env_memstick;
    }
    envp[env_count++] = env_tables;

    if (session->dataroot_path[0]) {
        snprintf(env_dataroot, sizeof(env_dataroot), "SR_DATAROOT=%s", session->dataroot_path);
        envp[env_count++] = env_dataroot;
    }
    if (session->font_dir[0]) {
        snprintf(env_font, sizeof(env_font), "SR_FONTDIR=%s", session->font_dir);
        envp[env_count++] = env_font;
    }

    if (session->config.benchmark_mode) {
        snprintf(env_debug, sizeof(env_debug), "SR_DEBUG=0x20");
        envp[env_count++] = env_debug;
    }

    if (session->config.diagnostic_mode) {
        snprintf(env_fatal, sizeof(env_fatal), "SR_DISPATCH_FATAL=1");
        envp[env_count++] = env_fatal;
    }
    /* The player launch smoke uses this opt-in side channel because a spawned
       runtime's stderr is not a stable API of either platform backend. Normal
       launches do not set it and therefore incur no extra file I/O. */
    const char *boot_event_path = getenv("SR_BOOT_EVENT_FILE");
    if (boot_event_path && *boot_event_path) {
        int w = snprintf(env_boot_event, sizeof(env_boot_event),
                         "SR_BOOT_EVENT_FILE=%s", boot_event_path);
        if (w > 0 && (size_t)w < sizeof(env_boot_event)) {
            envp[env_count++] = env_boot_event;
        }
    }
    envp[env_count] = NULL;

    /* Build command-line arguments */
    char base_str[32];
    char entry_str[32];
    snprintf(base_str, sizeof(base_str), "0x%x", session->base_address);
    snprintf(entry_str, sizeof(entry_str), "0x%08x", session->entry_point);

    const char *argv[12];
    int argc = 0;
    argv[argc++] = session->executable_path;
    session->argv_has_gui = false;

    if (session->image_path[0]) {
        argv[argc++] = "--image";
        argv[argc++] = session->image_path;
        argv[argc++] = base_str;
        argv[argc++] = entry_str;
        argv[argc++] = "none";
        argv[argc++] = "none";
        if (session->config.gui_mode) {
            argv[argc++] = "--gui";
            session->argv_has_gui = true;
        } else {
            argv[argc++] = "--sched";
        }
    } else {
        if (session->config.gui_mode) {
            argv[argc++] = "--gui";
            session->argv_has_gui = true;
        }
    }
    argv[argc] = NULL;

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
        /* The child is gone. Read its status HERE, while the answer still
           exists: on POSIX this poll is what reaped it, so the backend holds
           the status and a later waitpid would only see ECHILD; on Win32 the
           handle is still open. Without this, both polling sequences in
           src/player/main.c called nk_launch_wait on a session this function
           had just marked stopped, which returned the default exit_code and
           reported every runtime exit as 0. */
        session->exit_code = nk_platform_wait_process(&session->process, 0);
        session->is_running = false;
    }
    return running;
}

int nk_launch_wait(NkLaunchSession *session, int timeout_ms) {
    if (!session) return -1;
    if (!session->is_running) return session->exit_code;

    int code = nk_platform_wait_process(&session->process, timeout_ms);

    if (timeout_ms >= 0 && code == -1) {
        /* A finite wait that expires returns -1 with the child still alive and
           the handle still valid on both backends. Recording that as the exit
           code and marking the session stopped made a timeout unrecoverable:
           the caller could not wait again, and nk_launch_stop then skipped
           termination and discarded a still-live handle. */
        if (nk_platform_is_process_running(&session->process)) {
            return code;
        }
        /* It exited between the wait expiring and this check, so -1 was the
           timeout rather than the child's status. Ask once more without
           waiting, now that the backend has the answer. */
        code = nk_platform_wait_process(&session->process, 0);
    }

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
