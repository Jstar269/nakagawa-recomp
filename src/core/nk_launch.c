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
    if (session->iso_path[0]) {
        snprintf(env_iso, sizeof(env_iso), "PSP_ISO=%s", session->iso_path);
        envp[env_count++] = env_iso;
    }
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
