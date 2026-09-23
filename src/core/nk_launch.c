/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_launch.h"
#include "generated/nk_title_catalog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#define NK_LAUNCH_MAX_ELF_BYTES (256u * 1024u * 1024u)
#define NK_LAUNCH_MAX_PH_TABLE_BYTES (4u * 1024u * 1024u)
#define NK_LAUNCH_MAX_PROGRAM_HEADERS 4096u
#define NK_ELF_PT_LOAD 1u
#define NK_ELF_PF_X 1u

static inline void safe_copy_path(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0) return;
    if (!src) { dest[0] = '\0'; return; }
    strncpy(dest, src, dest_size - 1);
    dest[dest_size - 1] = '\0';
}

static uint16_t launch_read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t launch_read_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void launch_error(char *error_message, size_t error_message_size,
                         const char *message) {
    if (!error_message || error_message_size == 0) return;
    snprintf(error_message, error_message_size, "%s",
             message ? message : "launch validation failed");
}

static FILE *launch_fopen(const char *path, const char *mode) {
#if defined(_WIN32) || defined(_WIN64)
    if (!path || !mode) return NULL;
    WCHAR wpath[32768];
    WCHAR wmode[32];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1,
                            wmode, (int)(sizeof(wmode) / sizeof(wmode[0]))) <= 0) {
        return NULL;
    }
    return _wfopen(wpath, wmode);
#else
    return (path && mode) ? fopen(path, mode) : NULL;
#endif
}

static bool launch_join_path(const char *base, const char *relative,
                             char *out_path, size_t out_size) {
    if (!base || !base[0] || !relative || !relative[0] ||
        !out_path || out_size == 0) return false;
    if (relative[0] == '/' || relative[0] == '\\' ||
        (relative[0] && relative[1] == ':')) return false;
    int written = snprintf(out_path, out_size, "%s%c%s", base,
                           nk_platform_path_separator(), relative);
    return written >= 0 && (size_t)written < out_size;
}

static bool launch_store_existing(const char *candidate, bool directory,
                                  char *out_path, size_t out_size) {
    if (!candidate || !out_path || out_size == 0) return false;
    if (directory ? !nk_platform_dir_exists(candidate)
                  : !nk_platform_file_exists(candidate)) return false;
    char absolute[NK_MAX_PATH * 2];
    if (nk_platform_absolute_path(candidate, absolute, sizeof(absolute))) {
        if (strlen(absolute) >= out_size) return false;
        safe_copy_path(out_path, out_size, absolute);
    } else {
        if (strlen(candidate) >= out_size) return false;
        safe_copy_path(out_path, out_size, candidate);
    }
    return out_path[0] != '\0';
}

static bool launch_find_staged_executable(const NkGameEntry *game,
                                          char *out_path, size_t out_size) {
    if (!game || !game->prepared_root[0] || !out_path || out_size == 0) return false;
    char candidate[NK_MAX_PATH * 2];
    if (launch_join_path(game->prepared_root, "EBOOT.BIN",
                         candidate, sizeof(candidate)) &&
        launch_store_existing(candidate, false, out_path, out_size)) return true;
    if (launch_join_path(game->prepared_root, "PSP_GAME/SYSDIR/EBOOT.BIN",
                         candidate, sizeof(candidate)) &&
        launch_store_existing(candidate, false, out_path, out_size)) return true;
    return false;
}

static bool launch_find_existing_directory(const char *base, const char *relative,
                                           char *out_path, size_t out_size) {
    char candidate[NK_MAX_PATH * 2];
    return launch_join_path(base, relative, candidate, sizeof(candidate)) &&
           launch_store_existing(candidate, true, out_path, out_size);
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

/* #366: nk_launch.c binds the shared contract's name sources onto the
 * validated catalog entry through GENERATED index macros only
 * (NK_LAUNCH_NAME_SOURCE_<SOURCE>_INDEX, emitted by title_catalog_codegen.py
 * from tools/nk_core/launcher.py: NAME_SOURCES). A planner-side reorder
 * changes those indices and native ordering follows mechanically; losing a
 * required source, growing NAME_SOURCES, or aliasing two sources onto one
 * slot fails compilation instead of silently inventing an ordering. Extend
 * this mapping when NAME_SOURCES grows. */
#if NK_LAUNCH_NAME_SOURCE_COUNT != 2
#error "nk_launch.c maps (game_name, title_id); extend it when NAME_SOURCES grows"
#endif
#ifndef NK_LAUNCH_NAME_SOURCE_GAME_NAME_INDEX
#error "generated NAME_SOURCES lost 'game_name'; nk_launch.c cannot bind name sources"
#endif
#ifndef NK_LAUNCH_NAME_SOURCE_TITLE_ID_INDEX
#error "generated NAME_SOURCES lost 'title_id'; nk_launch.c cannot bind name sources"
#endif
#if NK_LAUNCH_NAME_SOURCE_GAME_NAME_INDEX >= NK_LAUNCH_NAME_SOURCE_COUNT || \
    NK_LAUNCH_NAME_SOURCE_TITLE_ID_INDEX >= NK_LAUNCH_NAME_SOURCE_COUNT || \
    NK_LAUNCH_NAME_SOURCE_GAME_NAME_INDEX == NK_LAUNCH_NAME_SOURCE_TITLE_ID_INDEX
#error "generated name-source indices are out of range or alias one another"
#endif

static void launch_bind_name_sources(
    const char *game_name,
    const char *title_id,
    const char *names[NK_LAUNCH_NAME_SOURCE_COUNT]
) {
    for (size_t i = 0; i < NK_LAUNCH_NAME_SOURCE_COUNT; i++) names[i] = NULL;
    names[NK_LAUNCH_NAME_SOURCE_GAME_NAME_INDEX] = game_name;
    names[NK_LAUNCH_NAME_SOURCE_TITLE_ID_INDEX] = title_id;
}

/* Select the ONE validated catalog entry that a session's identity names.
 *
 * disc_id and title_id, when both present, must agree on the same entry. A
 * half-resolved pair (one side names a catalog title, the other does not) or
 * a pair that resolves to two different titles is a session identity
 * disagreement and is rejected: letting one side win would pair a runtime
 * and addresses from one title with session data from another. An identity
 * that resolves to no catalog entry fails closed with the same diagnostic
 * the launch path has always reported for unknown titles. */
static const NkTitleEntry *launch_select_entry(const NkGameEntry *game,
                                               char *error_message,
                                               size_t error_message_size) {
    const NkTitleEntry *by_disc = NULL;
    const NkTitleEntry *by_id = NULL;
    if (!game) {
        launch_error(error_message, error_message_size,
                     "launch identity received invalid arguments");
        return NULL;
    }
    if (game->disc_id[0]) by_disc = nk_title_catalog_find_by_disc_id(game->disc_id);
    if (game->title_id[0]) by_id = nk_title_catalog_find_by_id(game->title_id);

    if (game->disc_id[0] && game->title_id[0]) {
        if (by_disc && by_id) {
            if (by_disc == by_id || strcmp(by_disc->id, by_id->id) == 0) {
                return by_disc;
            }
            if (error_message && error_message_size > 0) {
                snprintf(error_message, error_message_size,
                         "Session identity disagreement: disc_id '%.16s' resolves "
                         "to title '%.64s' but title_id '%.64s' resolves to title "
                         "'%.64s'",
                         game->disc_id, by_disc->id, game->title_id, by_id->id);
            }
            return NULL;
        }
        if (by_disc) {
            if (error_message && error_message_size > 0) {
                snprintf(error_message, error_message_size,
                         "Session identity disagreement: disc_id '%.16s' resolves "
                         "to title '%.64s' but title_id '%.64s' does not name a "
                         "catalog title",
                         game->disc_id, by_disc->id, game->title_id);
            }
            return NULL;
        }
        if (by_id) {
            if (error_message && error_message_size > 0) {
                snprintf(error_message, error_message_size,
                         "Session identity disagreement: title_id '%.64s' resolves "
                         "to title '%.64s' but disc_id '%.16s' does not name a "
                         "catalog disc",
                         game->title_id, by_id->id, game->disc_id);
            }
            return NULL;
        }
    } else if (by_disc) {
        return by_disc;
    } else if (by_id) {
        return by_id;
    }

    /* No validated identity: the historic unknown-title diagnostic, unchanged. */
    if (error_message && error_message_size > 0) {
        snprintf(error_message, error_message_size,
                 "No catalog entry describes this title (disc_id=%.16s title_id=%.32s)",
                 game->disc_id, game->title_id);
    }
    return NULL;
}

/* The executable's final two path components must BE the selected title's own
 * identity: <name>/<name>[.exe] for one of the entry's contract name sources.
 * This is the pre-spawn identity binding: a runtime produced for title A must
 * not launch as title B merely because its path exists. */
static bool launch_executable_bound_to_entry(const char *exe_path,
                                             const NkTitleEntry *entry) {
    char work[NK_MAX_PATH * 2];
    char expected[NK_MAX_PATH];
    char *file_sep = NULL;
    char *dir_sep = NULL;
    char *p;
    size_t len;
    const char *names[NK_LAUNCH_NAME_SOURCE_COUNT];

    if (!exe_path || !exe_path[0] || !entry) return false;
    snprintf(work, sizeof(work), "%s", exe_path);
    len = strlen(work);
    while (len > 1 && (work[len - 1] == '/' || work[len - 1] == '\\')) {
        work[--len] = '\0';
    }
    for (p = work; *p; p++) {
        if (*p == '/' || *p == '\\') file_sep = p;
    }
    if (!file_sep) return false;
    *file_sep = '\0';
    for (p = work; *p; p++) {
        if (*p == '/' || *p == '\\') dir_sep = p;
    }
    {
        const char *file_name = file_sep + 1;
        const char *dir_name = dir_sep ? dir_sep + 1 : work;
        if (!dir_name[0] || !file_name[0]) return false;
        launch_bind_name_sources(entry->game_name, entry->id, names);
        for (size_t i = 0; i < NK_LAUNCH_NAME_SOURCE_COUNT; i++) {
            const char *name = names[i];
            if (!name || !name[0]) continue;
            if (strcmp(dir_name, name) != 0) continue;
            if (strcmp(file_name, name) == 0) return true;
            snprintf(expected, sizeof(expected), "%s.exe", name);
            if (strcmp(file_name, expected) == 0) return true;
        }
    }
    return false;
}

/* Resolve the SELECTED TITLE's own runtime executable under `root`.
 *
 * Every candidate comes from the generated shared contract
 * (nk_launch_exe_candidates, projected from tools/nk_core/launcher.py by
 * tools/title_catalog_codegen.py) instantiated with the validated catalog
 * entry's game_name and title id in the contract's name-source order. `root`
 * is a directory by contract. There is no sibling-title, retail-title, or
 * other rescue path: a stale build of a different title in this workspace is
 * irrelevant, and an unresolved runtime is an honest missing-runtime error.
 */
static bool find_candidate_executable(
    const char *root,
    const char *game_name,
    const char *title_id,
    char *out_path,
    size_t max_len
) {
    char sep;
    char rel[NK_MAX_PATH];
    char cand[NK_MAX_PATH * 2];
    const char *names[NK_LAUNCH_NAME_SOURCE_COUNT];

    if (!root || !root[0] || !out_path || max_len == 0) return false;
    sep = nk_platform_path_separator();
    launch_bind_name_sources(game_name, title_id, names);
    for (size_t ni = 0; ni < NK_LAUNCH_NAME_SOURCE_COUNT; ni++) {
        const char *name = names[ni];
        if (!name || !name[0]) continue;
        for (int ci = 0; ci < NK_LAUNCH_EXE_CANDIDATE_COUNT; ci++) {
            int rw = snprintf(rel, sizeof(rel), nk_launch_exe_candidates[ci],
                              name, name);
            if (rw <= 0 || (size_t)rw >= sizeof(rel)) continue;
            for (int k = 0; k < rw; k++) {
                if (rel[k] == '/') rel[k] = sep;
            }
            int w = snprintf(cand, sizeof(cand), "%s%c%s", root, sep, rel);
            if (w > 0 && (size_t)w < sizeof(cand) &&
                nk_platform_file_exists(cand)) {
                if ((size_t)w >= max_len) return false;
                snprintf(out_path, max_len, "%s", cand);
                return true;
            }
        }
    }
    return false;
}

bool nk_launch_runtime_available(const char *root, const char *title_id) {
    char resolved[NK_MAX_PATH];
    const char *effective_root = (root && *root) ? root : ".";
    const NkTitleEntry *entry =
        (title_id && *title_id) ? nk_title_catalog_find_by_id(title_id) : NULL;
    if (!entry) return false;
    return find_candidate_executable(effective_root, entry->game_name, entry->id,
                                     resolved, sizeof(resolved));
}

NkResult nk_launch_validate_staged_executable(const NkGameEntry *game,
                                              const NkTitleEntry *manifest,
                                              NkLaunchExecutableInfo *out_info,
                                              char *error_message,
                                              size_t error_message_size) {
    if (error_message && error_message_size > 0) error_message[0] = '\0';
    if (out_info) memset(out_info, 0, sizeof(*out_info));
    if (!game || !manifest || !out_info) {
        launch_error(error_message, error_message_size,
                     "staged executable validation received invalid arguments");
        return NK_ERROR_GENERIC;
    }
    if (manifest->bss_metadata_source &&
        strcmp(manifest->bss_metadata_source, "elf") != 0 &&
        strcmp(manifest->bss_metadata_source, "psp-header") != 0 &&
        strcmp(manifest->bss_metadata_source, "none") != 0) {
        launch_error(error_message, error_message_size,
                     "title manifest has an unsupported BSS metadata source");
        return NK_ERROR_INVALID_EXECUTABLE;
    }

    char path[NK_MAX_PATH];
    if (!launch_find_staged_executable(game, path, sizeof(path))) {
        launch_error(error_message, error_message_size,
                     "staged EBOOT.BIN was not found under the promoted game root");
        return NK_ERROR_FILE_NOT_FOUND;
    }
    int64_t raw_size = nk_platform_get_file_size(path);
    if (raw_size <= 0 || (uint64_t)raw_size > NK_LAUNCH_MAX_ELF_BYTES) {
        launch_error(error_message, error_message_size,
                     "staged EBOOT.BIN is empty or exceeds the executable size budget");
        return NK_ERROR_INVALID_EXECUTABLE;
    }

    FILE *file = launch_fopen(path, "rb");
    if (!file) {
        launch_error(error_message, error_message_size,
                     "staged EBOOT.BIN could not be opened for validation");
        return NK_ERROR_IO;
    }

    uint8_t header[0x80] = { 0 };
    size_t header_bytes = (uint64_t)raw_size < sizeof(header)
        ? (size_t)raw_size : sizeof(header);
    if (header_bytes < 52 || fread(header, 1, header_bytes, file) != header_bytes) {
        fclose(file);
        launch_error(error_message, error_message_size,
                     "staged EBOOT.BIN has a truncated executable header");
        return NK_ERROR_INVALID_EXECUTABLE;
    }

    /* Retail PSP EBOOT.BIN files are commonly ~PSP containers. Their inner
     * ELF is encrypted until the separate lawful decryption phase, so the
     * native launcher records the container boundary without pretending it
     * validated inner program headers. */
    if (memcmp(header, "~PSP", 4) == 0) {
        /* Offset 4 is the module attribute/version word, not a header length.
         * Validate the PSP fields that describe the in-memory module instead:
         * the segment count lives at 0x27, the aggregate BSS size at 0x38,
         * and the four segment memory sizes begin at 0x54. */
        if (header_bytes < 0x64 || header[0x27] == 0 || header[0x27] > 4) {
            fclose(file);
            launch_error(error_message, error_message_size,
                         "staged PSP executable container has an invalid segment table");
            return NK_ERROR_INVALID_EXECUTABLE;
        }
        uint64_t segment_memory = 0;
        for (unsigned i = 0; i < header[0x27]; i++) {
            uint32_t segment_size = launch_read_u32(header + 0x54 + i * 4u);
            if (segment_size == 0 || segment_size > NK_LAUNCH_MAX_ELF_BYTES ||
                segment_memory > NK_LAUNCH_MAX_ELF_BYTES - segment_size) {
                fclose(file);
                launch_error(error_message, error_message_size,
                             "staged PSP executable container has an invalid segment size");
                return NK_ERROR_INVALID_EXECUTABLE;
            }
            segment_memory += segment_size;
        }
        uint32_t bss_size = launch_read_u32(header + 0x38);
        if ((uint64_t)bss_size > segment_memory ||
            bss_size > NK_LAUNCH_MAX_ELF_BYTES) {
            fclose(file);
            launch_error(error_message, error_message_size,
                         "staged PSP executable container has invalid BSS metadata");
            return NK_ERROR_INVALID_EXECUTABLE;
        }
        out_info->is_psp_container = true;
        fclose(file);
        return NK_OK;
    }

    if (memcmp(header, "\x7f" "ELF", 4) != 0 || header[4] != 1 ||
        header[5] != 1 || header[6] != 1) {
        fclose(file);
        launch_error(error_message, error_message_size,
                     "staged executable is neither a PSP container nor ELF32 little-endian data");
        return NK_ERROR_INVALID_EXECUTABLE;
    }
    if (manifest->bss_metadata_source &&
        strcmp(manifest->bss_metadata_source, "psp-header") == 0) {
        fclose(file);
        launch_error(error_message, error_message_size,
                     "title manifest requires PSP-header BSS metadata, but staged input is a bare ELF");
        return NK_ERROR_INVALID_EXECUTABLE;
    }
    uint16_t elf_type = launch_read_u16(header + 16);
    uint16_t machine = launch_read_u16(header + 18);
    if ((elf_type != 2 && elf_type != 3) || machine != 8 ||
        launch_read_u32(header + 20) != 1 || launch_read_u16(header + 40) < 52) {
        fclose(file);
        launch_error(error_message, error_message_size,
                     "staged ELF must be an ET_EXEC/ET_DYN MIPS32 image");
        return NK_ERROR_INVALID_EXECUTABLE;
    }

    uint32_t entry_point = launch_read_u32(header + 24);
    uint32_t program_header_offset = launch_read_u32(header + 28);
    uint16_t program_header_size = launch_read_u16(header + 42);
    uint16_t program_header_count = launch_read_u16(header + 44);
    if (program_header_count == 0 || program_header_count > NK_LAUNCH_MAX_PROGRAM_HEADERS ||
        program_header_size < 32 || (entry_point & 3u) != 0) {
        fclose(file);
        launch_error(error_message, error_message_size,
                     "staged ELF has no usable program-header table");
        return NK_ERROR_INVALID_EXECUTABLE;
    }
    uint64_t table_size = (uint64_t)program_header_size * program_header_count;
    if (table_size > NK_LAUNCH_MAX_PH_TABLE_BYTES ||
        (uint64_t)program_header_offset > (uint64_t)raw_size ||
        table_size > (uint64_t)raw_size - program_header_offset) {
        fclose(file);
        launch_error(error_message, error_message_size,
                     "staged ELF program-header table is outside the file bounds");
        return NK_ERROR_INVALID_EXECUTABLE;
    }

    uint8_t *program_headers = (uint8_t *)malloc((size_t)table_size);
    uint32_t *load_starts = (uint32_t *)calloc(program_header_count, sizeof(*load_starts));
    uint32_t *load_ends = (uint32_t *)calloc(program_header_count, sizeof(*load_ends));
    NkResult result = NK_ERROR_INVALID_EXECUTABLE;
    if (!program_headers || !load_starts || !load_ends) {
        launch_error(error_message, error_message_size,
                     "out of memory while validating staged ELF program headers");
        result = NK_ERROR_OUT_OF_MEMORY;
        goto staged_elf_cleanup;
    }
    if (nk_fseek64(file, (int64_t)program_header_offset, SEEK_SET) != 0 ||
        fread(program_headers, 1, (size_t)table_size, file) != (size_t)table_size) {
        launch_error(error_message, error_message_size,
                     "staged ELF program-header table could not be read");
        goto staged_elf_cleanup;
    }

    uint64_t load_base = UINT64_MAX;
    uint64_t image_end = 0;
    uint64_t file_backed_end = 0;
    uint64_t bss_start = UINT64_MAX;
    uint64_t bss_end = 0;
    size_t load_count = 0;
    bool entry_in_executable_segment = false;
    for (uint16_t i = 0; i < program_header_count; i++) {
        const uint8_t *ph = program_headers + (size_t)i * program_header_size;
        uint32_t type = launch_read_u32(ph + 0);
        if (type != NK_ELF_PT_LOAD) continue;

        uint32_t source_offset = launch_read_u32(ph + 4);
        uint32_t virtual_address = launch_read_u32(ph + 8);
        uint32_t file_size = launch_read_u32(ph + 16);
        uint32_t memory_size = launch_read_u32(ph + 20);
        uint32_t flags = launch_read_u32(ph + 24);
        uint32_t alignment = launch_read_u32(ph + 28);
        if (memory_size == 0 || file_size > memory_size ||
            (uint64_t)source_offset > (uint64_t)raw_size ||
            (uint64_t)file_size > (uint64_t)raw_size - source_offset) {
            launch_error(error_message, error_message_size,
                         "staged ELF PT_LOAD has invalid file or memory bounds");
            goto staged_elf_cleanup;
        }
        uint64_t memory_end = (uint64_t)virtual_address + memory_size;
        if (memory_end > (uint64_t)UINT32_MAX ||
            (alignment > 1 && (alignment & (alignment - 1u)) != 0) ||
            (alignment > 1 && ((uint64_t)source_offset % alignment) !=
                              ((uint64_t)virtual_address % alignment))) {
            launch_error(error_message, error_message_size,
                         "staged ELF PT_LOAD has invalid address or alignment geometry");
            goto staged_elf_cleanup;
        }
        if (load_count >= program_header_count) {
            launch_error(error_message, error_message_size,
                         "staged ELF contains too many load segments");
            goto staged_elf_cleanup;
        }
        load_starts[load_count] = virtual_address;
        load_ends[load_count] = (uint32_t)memory_end;
        load_count++;
        if ((uint64_t)virtual_address < load_base) load_base = virtual_address;
        if (memory_end > image_end) image_end = memory_end;
        uint64_t segment_file_end = (uint64_t)virtual_address + file_size;
        if (segment_file_end > file_backed_end) file_backed_end = segment_file_end;
        if (file_size < memory_size) {
            uint64_t segment_bss_start = (uint64_t)virtual_address + file_size;
            if (segment_bss_start < bss_start) bss_start = segment_bss_start;
            if (memory_end > bss_end) bss_end = memory_end;
        }
        if ((flags & NK_ELF_PF_X) != 0 && entry_point >= virtual_address &&
            (uint64_t)entry_point < memory_end) {
            entry_in_executable_segment = true;
        }
    }
    if (load_count == 0 || !entry_in_executable_segment) {
        launch_error(error_message, error_message_size,
                     load_count == 0 ? "staged ELF contains no PT_LOAD segments"
                                     : "staged ELF entry is not inside an executable PT_LOAD segment");
        goto staged_elf_cleanup;
    }
    for (size_t left = 0; left < load_count; left++) {
        for (size_t right = left + 1; right < load_count; right++) {
            if (load_starts[left] < load_ends[right] &&
                load_starts[right] < load_ends[left]) {
                launch_error(error_message, error_message_size,
                             "staged ELF PT_LOAD guest ranges overlap");
                goto staged_elf_cleanup;
            }
        }
    }
    if (manifest->executable_base != 0 && load_base != manifest->executable_base) {
        launch_error(error_message, error_message_size,
                     "staged ELF load base does not match the title manifest");
        goto staged_elf_cleanup;
    }
    if (manifest->executable_entry != 0 && entry_point != manifest->executable_entry) {
        launch_error(error_message, error_message_size,
                     "staged ELF entry point does not match the title manifest");
        goto staged_elf_cleanup;
    }

    out_info->is_elf = true;
    out_info->entry_in_executable_segment = entry_in_executable_segment;
    out_info->has_bss = bss_start != UINT64_MAX;
    out_info->load_base = (uint32_t)load_base;
    out_info->image_end = (uint32_t)image_end;
    out_info->file_backed_end = (uint32_t)file_backed_end;
    out_info->bss_start = (uint32_t)(out_info->has_bss ? bss_start : image_end);
    out_info->bss_end = (uint32_t)(out_info->has_bss ? bss_end : image_end);
    out_info->program_header_count = program_header_count;
    out_info->load_segment_count = (uint16_t)load_count;
    result = NK_OK;

staged_elf_cleanup:
    free(load_ends);
    free(load_starts);
    free(program_headers);
    fclose(file);
    return result;
}

/* Resolve the SELECTED TITLE's own runtime image under `working_dir`.
 *
 * Order (shared with the Python planner, #366):
 *   1. the sibling of the resolved executable: <stem><IMAGE_SUFFIX>;
 *   2. the identity-derived build layouts from the generated contract,
 *      instantiated with the validated entry's name sources in order.
 *
 * There is no sibling-title image probe and no title-specific image
 * fallback: a selected title without its own image fails closed, because a
 * session started without --image exits through src/rt/driver.c's usage path.
 *
 * Only a dot in the FINAL path component can be an extension:
 * /opt/nakagawa.d/bin/my-title has a dot but no extension, and a leading
 * dot (.hidden) names the file rather than separating an extension. */
static bool find_candidate_image(
    const char *working_dir,
    const char *executable_path,
    const char *game_name,
    const char *title_id,
    char *out_path,
    size_t max_len
) {
    char cand[NK_MAX_PATH * 2];
    char rel[NK_MAX_PATH];
    char sep = nk_platform_path_separator();
    const char *names[NK_LAUNCH_NAME_SOURCE_COUNT];

    if (!out_path || max_len == 0) return false;

    /* 1. Alongside executable: <executable without extension><IMAGE_SUFFIX> */
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
        if (used + sizeof(NK_LAUNCH_IMAGE_SUFFIX) <= sizeof(cand)) {
            snprintf(suffix_at, sizeof(cand) - used, NK_LAUNCH_IMAGE_SUFFIX);
            if (nk_platform_file_exists(cand)) {
                if (strlen(cand) >= max_len) return false;
                snprintf(out_path, max_len, "%s", cand);
                return true;
            }
        }
    }

    /* 2. Identity-derived build layouts from the shared contract. */
    if (working_dir && working_dir[0]) {
        launch_bind_name_sources(game_name, title_id, names);
        for (size_t ni = 0; ni < NK_LAUNCH_NAME_SOURCE_COUNT; ni++) {
            const char *name = names[ni];
            if (!name || !name[0]) continue;
            for (int ci = 0; ci < NK_LAUNCH_IMAGE_CANDIDATE_COUNT; ci++) {
                int rw = snprintf(rel, sizeof(rel),
                                  nk_launch_image_candidates[ci], name, name);
                if (rw <= 0 || (size_t)rw >= sizeof(rel)) continue;
                for (int k = 0; k < rw; k++) {
                    if (rel[k] == '/') rel[k] = sep;
                }
                int w = snprintf(cand, sizeof(cand), "%s%c%s", working_dir,
                                 sep, rel);
                if (w > 0 && (size_t)w < sizeof(cand) &&
                    nk_platform_file_exists(cand)) {
                    if ((size_t)w >= max_len) return false;
                    snprintf(out_path, max_len, "%s", cand);
                    return true;
                }
            }
        }
    }

    return false;
}

bool nk_launch_runtime_package_available(const char *root, const char *title_id) {
    const char *effective_root = (root && *root) ? root : ".";
    const NkTitleEntry *entry =
        (title_id && *title_id) ? nk_title_catalog_find_by_id(title_id) : NULL;
    char executable[NK_MAX_PATH], image[NK_MAX_PATH];
    if (!entry ||
        !find_candidate_executable(effective_root, entry->game_name, entry->id,
                                   executable, sizeof(executable))) {
        return false;
    }
    return find_candidate_image(effective_root, executable, entry->game_name,
                                entry->id, image, sizeof(image));
}

NkResult nk_launch_prepare_session(
    NkLaunchSession *session,
    const NkGameEntry *game,
    const char *repo_or_install_root
) {
    if (!session || !game) return NK_ERROR_GENERIC;
    memset(session, 0, sizeof(*session));

    const char *root = (repo_or_install_root && *repo_or_install_root) ? repo_or_install_root : ".";
    /* #366: `root` is a directory by contract. A direct file path is not an
       escape hatch in the generic API: handing the launcher an exact legacy
       binary path does not make it a validated runtime for this title, and
       deriving the working directory from a file root only existed to
       support that path. Assets resolve relative to the root the caller
       named. */
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
    session->config.gui_mode = false;

    /* Resolve the ONE validated catalog entry this session identity names,
       BEFORE executable discovery (#366). The native launch route uses the
       same manifest-selected game_name as the manager, bound to the same
       validated identity: disc_id and title_id must agree, and an identity
       the catalog does not describe fails closed here -- with the historic
       unknown-title diagnostic -- rather than after probing paths. */
    char identity_error[256];
    const NkTitleEntry *entry = launch_select_entry(game, identity_error,
                                                    sizeof(identity_error));
    if (!entry) {
        snprintf(session->last_error, sizeof(session->last_error), "%s",
                 identity_error);
        return NK_ERROR_UNSUPPORTED_TITLE;
    }
    const char *game_name = entry->game_name;

    /* 1. Resolve executable: the selected title's own build only. Candidates
       come from the shared generated contract; a stale build of another
       title in this workspace is irrelevant, and a missing runtime is an
       honest error rather than a wrong-title rescue. */
    if (!find_candidate_executable(root, game_name, entry->id,
                                   session->executable_path,
                                   sizeof(session->executable_path))) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "Runtime binary not found for title %.64s under root: %.120s",
                 entry->id, root);
        return NK_ERROR_FILE_NOT_FOUND;
    }

    /* 2. Resolve the selected title's own image.bin. A session without one is
       not a launch plan: spawning would only reach src/rt/driver.c's usage
       exit, so this fails closed exactly like the Python planner. */
    if (!find_candidate_image(session->working_directory,
                              session->executable_path, game_name, entry->id,
                              session->image_path,
                              sizeof(session->image_path))) {
        snprintf(session->last_error, sizeof(session->last_error),
                 "Runtime image not found for title %.64s under root: %.120s",
                 entry->id, session->working_directory);
        return NK_ERROR_FILE_NOT_FOUND;
    }

    /* 3. Addresses from the same validated entry that supplied the identity.
       Catalog membership is the validation: an undescribed title already
       failed in launch_select_entry (this is where the old hard-coded
       0x0029a060 fallback fabricated a launch instead). A declared zero
       base/entry is the manifest's own launch value -- legacy retail titles
       launch at 0 0 as documented -- not an unknown-address guess, and the
       Python planner reads the same fields identically. */
    session->base_address = entry->executable_base;
    session->entry_point = entry->executable_entry;

    /* A completed native staging transaction carries the source EBOOT under
     * prepared_root. Validate it before any launch environment is assembled;
     * this keeps a malformed or mismatched staged image from being mistaken
     * for a usable runtime input. PSP ~PSP containers are accepted only as a
     * bounded container boundary; their encrypted inner ELF is a later
     * decryption capability. */
    if (game->assets_staged) {
        if (!launch_find_staged_executable(game, session->staged_executable_path,
                                           sizeof(session->staged_executable_path))) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "Staged title %s has no EBOOT.BIN under prepared_root",
                     session->disc_id);
            return NK_ERROR_FILE_NOT_FOUND;
        }
        char staged_error[256];
        NkLaunchExecutableInfo staged_info;
        NkResult staged_result = nk_launch_validate_staged_executable(
            game, entry, &staged_info, staged_error, sizeof(staged_error));
        if (staged_result != NK_OK) {
            snprintf(session->last_error, sizeof(session->last_error), "%s",
                     staged_error[0] ? staged_error : "Staged executable validation failed");
            return staged_result;
        }
        session->staged_executable_checked = true;
        session->staged_executable_info = staged_info;
    }

    /* 4. Resolve data root */
    char sep = nk_platform_path_separator();
    if (game->assets_staged && game->prepared_root[0]) {
        /* Native staging deliberately writes the decoded XB tree below
         * prepared_root/xbdata. Prefer a manifest-relative root when it is
         * present (useful for future staged layouts), then the canonical
         * native staging names. */
        if (entry && entry->data_root) {
            launch_find_existing_directory(game->prepared_root, entry->data_root,
                                           session->dataroot_path,
                                           sizeof(session->dataroot_path));
        }
        if (session->dataroot_path[0] == '\0') {
            launch_find_existing_directory(game->prepared_root, "xbdata",
                                           session->dataroot_path,
                                           sizeof(session->dataroot_path));
        }
        if (session->dataroot_path[0] == '\0') {
            launch_find_existing_directory(game->prepared_root, "xbdata_extracted",
                                           session->dataroot_path,
                                           sizeof(session->dataroot_path));
        }
    }
    if (session->dataroot_path[0] == '\0' && entry && entry->data_root) {
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
    if (game->assets_staged && game->prepared_root[0]) {
        char cand_ms[NK_MAX_PATH * 2];
        if (launch_join_path(game->prepared_root, "memstick",
                             cand_ms, sizeof(cand_ms)) &&
            ensure_writable_dir(cand_ms)) {
            char absolute[NK_MAX_PATH * 2];
            if (nk_platform_absolute_path(cand_ms, absolute, sizeof(absolute))) {
                safe_copy_path(session->memstick_root, sizeof(session->memstick_root), absolute);
            } else {
                safe_copy_path(session->memstick_root, sizeof(session->memstick_root), cand_ms);
            }
        }
    }
    if (session->memstick_root[0] == 0 && entry && entry->memory_stick_root && *entry->memory_stick_root) {
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
        } else if (game->assets_staged && session->staged_executable_path[0]) {
            /* The promoted EBOOT and data root are enough for a staged launch
             * preparation. Leave PSP_ISO unset rather than relabelling the
             * executable as an ISO. */
            session->iso_path[0] = '\0';
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

    /* Identity binding immediately before spawn (#366): the session must
       still name a catalog-resolved title, and the resolved executable must
       BE that title's own identity-shaped build. A runtime produced for
       title A must not launch as title B merely because its path exists, and
       a session whose executable or identity was swapped after preparation
       is refused here rather than spawned. */
    {
        NkGameEntry identity;
        char gate_error[256];
        memset(&identity, 0, sizeof(identity));
        snprintf(identity.disc_id, sizeof(identity.disc_id), "%s",
                 session->disc_id);
        snprintf(identity.title_id, sizeof(identity.title_id), "%s",
                 session->title_id);
        const NkTitleEntry *bound_entry = launch_select_entry(
            &identity, gate_error, sizeof(gate_error));
        if (!bound_entry) {
            snprintf(session->last_error, sizeof(session->last_error), "%s",
                     gate_error);
            return NK_ERROR_UNSUPPORTED_TITLE;
        }
        if (!launch_executable_bound_to_entry(session->executable_path,
                                              bound_entry)) {
            snprintf(session->last_error, sizeof(session->last_error),
                     "Resolved executable does not match the selected title "
                     "'%.48s': %.140s",
                     bound_entry->id, session->executable_path);
            return NK_ERROR_INVALID_EXECUTABLE;
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
    char env_modules[NK_MAX_PATH * 2 + 32];
    char env_tables[64];
    char env_boot_event[NK_MAX_PATH + 32];
    char sep = nk_platform_path_separator();

    snprintf(env_fps, sizeof(env_fps), "SR_FPS_CAP=%d", session->config.fps_cap);
    snprintf(env_ge, sizeof(env_ge), "SR_GPU_GE=1");
    snprintf(env_scale, sizeof(env_scale), "SR_RESOLUTION_SCALE=%d", session->config.resolution_scale);
    snprintf(env_vsync, sizeof(env_vsync), "SR_VSYNC=%d", session->config.vsync ? 1 : 0);
    snprintf(env_fs, sizeof(env_fs), "SR_FSDIR=fs");
    snprintf(env_memstick, sizeof(env_memstick), "SR_MEMSTICK=%s", session->memstick_root);
    snprintf(env_tables, sizeof(env_tables), "PSP_VFPU_TABLES=assets%cvfpu", sep);

    const char *envp[24];
    int env_count = 0;
    /* An empty value masks an inherited ISO for staged sessions with no ISO. */
    snprintf(env_iso, sizeof(env_iso), "PSP_ISO=%s", session->iso_path);
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

    /* A setup transaction promotes already-decrypted support modules below
     * the selected game's private root. Tell the runtime's late-import loader
     * where that exact tree lives; otherwise it falls back to the repository
     * development path and silently ignores the modules just staged. */
    if (session->prepared_root[0]) {
        char module_dir[NK_MAX_PATH * 2];
        if (launch_join_path(session->prepared_root, "EXTRACTED/decrypted",
                             module_dir, sizeof(module_dir))) {
            int module_written = snprintf(env_modules, sizeof(env_modules),
                                          "SR_MODULE_DIR=%s", module_dir);
            if (module_written > 0 && (size_t)module_written < sizeof(env_modules)) {
                envp[env_count++] = env_modules;
            }
        }
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
