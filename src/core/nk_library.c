/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#define _POSIX_C_SOURCE 200809L

#include "nk_library.h"
#include "nk_platform.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

static FILE *nk_lib_fopen(const char *path, const char *mode) {
#if defined(_WIN32) || defined(_WIN64)
    WCHAR wpath[32768];
    WCHAR wmode[32];
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 32768);
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wmode, 32);
    return _wfopen(wpath, wmode);
#else
    return fopen(path, mode);
#endif
}

void nk_library_init(NkLibrary *lib) {
    if (!lib) return;
    memset(lib, 0, sizeof(*lib));
}

int nk_library_count(const NkLibrary *lib) {
    return lib ? lib->count : 0;
}

const NkGameEntry *nk_library_get(const NkLibrary *lib, int index) {
    if (!lib || index < 0 || index >= lib->count) return NULL;
    return &lib->entries[index];
}

const NkGameEntry *nk_library_find_by_disc_id(const NkLibrary *lib, const char *disc_id) {
    if (!lib || !disc_id || !*disc_id) return NULL;
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].disc_id, disc_id) == 0) {
            return &lib->entries[i];
        }
    }
    return NULL;
}

NkResult nk_library_add_or_update(NkLibrary *lib, const NkGameEntry *entry) {
    if (!lib || !entry || entry->disc_id[0] == '\0') return NK_ERROR_GENERIC;

    /* Check if already exists */
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].disc_id, entry->disc_id) == 0) {
            lib->entries[i] = *entry;
            return NK_OK;
        }
    }

    /* Add new */
    if (lib->count >= NK_MAX_GAMES) {
        return NK_ERROR_OUT_OF_MEMORY;
    }

    lib->entries[lib->count++] = *entry;
    return NK_OK;
}

NkResult nk_library_remove(NkLibrary *lib, const char *disc_id) {
    if (!lib || !disc_id || !*disc_id) return NK_ERROR_GENERIC;

    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].disc_id, disc_id) == 0) {
            for (int j = i; j < lib->count - 1; j++) {
                lib->entries[j] = lib->entries[j + 1];
            }
            lib->count--;
            memset(&lib->entries[lib->count], 0, sizeof(NkGameEntry));
            return NK_OK;
        }
    }
    return NK_ERROR_FILE_NOT_FOUND;
}

/* Escape string for JSON */
static void escape_json_string(char *dest, size_t dest_size, const char *src) {
    if (!dest || dest_size == 0) return;
    size_t d = 0;
    for (size_t s = 0; src && src[s] && d + 2 < dest_size; s++) {
        if (src[s] == '\"' || src[s] == '\\') {
            dest[d++] = '\\';
            dest[d++] = src[s];
        } else if (src[s] == '\n') {
            dest[d++] = '\\';
            dest[d++] = 'n';
        } else if (src[s] == '\r') {
            dest[d++] = '\\';
            dest[d++] = 'r';
        } else if (src[s] == '\t') {
            dest[d++] = '\\';
            dest[d++] = 't';
        } else {
            dest[d++] = src[s];
        }
    }
    dest[d] = '\0';
}

NkResult nk_library_save(const NkLibrary *lib, const char *file_path) {
    if (!lib) return NK_ERROR_GENERIC;

    char default_path[NK_MAX_PATH];
    const char *target = file_path ? file_path : (lib->library_path[0] ? lib->library_path : NULL);
    if (!target) {
        if (!nk_platform_get_path(NK_PATH_DATA, default_path, sizeof(default_path))) {
            return NK_ERROR_IO;
        }
        int written = snprintf(default_path + strlen(default_path), sizeof(default_path) - strlen(default_path), "%clibrary.json", nk_platform_path_separator());
        if (written < 0) return NK_ERROR_IO;
        target = default_path;
    }

    char tmp_path[NK_MAX_PATH + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", target);

    FILE *f = nk_lib_fopen(tmp_path, "wb");
    if (!f) return NK_ERROR_IO;

    fprintf(f, "{\n  \"schema_version\": 1,\n  \"games\": [\n");

    for (int i = 0; i < lib->count; i++) {
        const NkGameEntry *g = &lib->entries[i];
        char esc_title[NK_MAX_TITLE_LEN * 2];
        char esc_iso[NK_MAX_PATH * 2];
        char esc_prep[NK_MAX_PATH * 2];

        escape_json_string(esc_title, sizeof(esc_title), g->title_name);
        escape_json_string(esc_iso, sizeof(esc_iso), g->iso_path);
        escape_json_string(esc_prep, sizeof(esc_prep), g->prepared_root);

        fprintf(f, "    {\n");
        fprintf(f, "      \"disc_id\": \"%s\",\n", g->disc_id);
        fprintf(f, "      \"title_name\": \"%s\",\n", esc_title);
        fprintf(f, "      \"disc_version\": \"%s\",\n", g->disc_version);
        fprintf(f, "      \"iso_path\": \"%s\",\n", esc_iso);
        fprintf(f, "      \"prepared_root\": \"%s\",\n", esc_prep);
        fprintf(f, "      \"title_id\": \"%s\",\n", g->title_id);
        fprintf(f, "      \"iso_size_bytes\": %llu,\n", (unsigned long long)g->iso_size_bytes);
        fprintf(f, "      \"status\": %d,\n", (int)g->status);
        fprintf(f, "      \"is_prepared\": %s,\n", g->is_prepared ? "true" : "false");
        fprintf(f, "      \"last_played\": \"%s\"\n", g->last_played);
        fprintf(f, "    }%s\n", (i < lib->count - 1) ? "," : "");
    }

    fprintf(f, "  ]\n}\n");
    if (fflush(f) != 0) {
        fclose(f);
#if defined(_WIN32) || defined(_WIN64)
        WCHAR wtmp[32768];
        MultiByteToWideChar(CP_UTF8, 0, tmp_path, -1, wtmp, 32768);
        DeleteFileW(wtmp);
#else
        remove(tmp_path);
#endif
        return NK_ERROR_IO;
    }

#if defined(_WIN32) || defined(_WIN64)
    int fd = _fileno(f);
    if (fd >= 0) {
        HANDLE hFile = (HANDLE)_get_osfhandle(fd);
        if (hFile != INVALID_HANDLE_VALUE) {
            FlushFileBuffers(hFile);
        }
    }
#else
    int fd = fileno(f);
    if (fd >= 0) {
        fsync(fd);
    }
#endif
    fclose(f);

    /* Atomic rename / replace with .bak backup preservation */
    char bak_path[NK_MAX_PATH + 8];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", target);

#if defined(_WIN32) || defined(_WIN64)
    WCHAR wtmp[32768];
    WCHAR wtarget[32768];
    WCHAR wbak[32768];
    MultiByteToWideChar(CP_UTF8, 0, tmp_path, -1, wtmp, 32768);
    MultiByteToWideChar(CP_UTF8, 0, target, -1, wtarget, 32768);
    MultiByteToWideChar(CP_UTF8, 0, bak_path, -1, wbak, 32768);

    if (nk_platform_file_exists(target)) {
        CopyFileW(wtarget, wbak, FALSE);
        if (!ReplaceFileW(wtarget, wtmp, NULL, REPLACEFILE_IGNORE_MERGE_ERRORS, NULL, NULL)) {
            if (!MoveFileExW(wtmp, wtarget, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                DeleteFileW(wtmp);
                return NK_ERROR_IO;
            }
        }
    } else {
        if (!MoveFileExW(wtmp, wtarget, MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(wtmp);
            return NK_ERROR_IO;
        }
    }
#else
    if (nk_platform_file_exists(target)) {
        /* Copy through a temporary file and promote only after a complete,
           verified copy. Opening bak_path directly truncated the previous
           backup before a single byte was written, and short writes, read
           errors and the close result were all discarded -- so one I/O error
           destroyed the last recovery image while this function still returned
           NK_OK, and a later corruption of the primary had nothing to fall back
           to. A failed copy now leaves the existing backup exactly as it was. */
        char bak_tmp[NK_MAX_PATH + 16];
        int bw = snprintf(bak_tmp, sizeof(bak_tmp), "%s.new", bak_path);
        if (bw > 0 && (size_t)bw < sizeof(bak_tmp)) {
            FILE *src = fopen(target, "rb");
            if (src) {
                FILE *dst = fopen(bak_tmp, "wb");
                bool copied = false;
                if (dst) {
                    char copy_buf[4096];
                    size_t n;
                    copied = true;
                    while ((n = fread(copy_buf, 1, sizeof(copy_buf), src)) > 0) {
                        if (fwrite(copy_buf, 1, n, dst) != n) {
                            copied = false;
                            break;
                        }
                    }
                    if (ferror(src)) copied = false;
                    if (fclose(dst) != 0) copied = false;
                }
                if (ferror(src)) copied = false;
                fclose(src);
                if (copied && rename(bak_tmp, bak_path) == 0) {
                    /* The new backup is in place. */
                } else {
                    remove(bak_tmp);
                }
            }
        }
    }

    if (rename(tmp_path, target) != 0) {
        remove(tmp_path);
        return NK_ERROR_IO;
    }
#endif

    return NK_OK;
}

/* Minimal JSON key-value extraction helper */
static const char *skip_whitespace(const char *p) {
    if (!p) return NULL;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

/* Bounded string value parser with overflow draining and unterminated string check */
static const char *parse_string_val(const char *p, char *out_val, size_t max_len) {
    if (!out_val || max_len == 0) return NULL;
    out_val[0] = '\0';
    p = skip_whitespace(p);
    if (!p || *p != '\"') return NULL;
    p++;
    size_t idx = 0;
    while (*p && *p != '\"') {
        if (*p == '\n' || *p == '\r') {
            /* Raw unescaped control character in JSON string is invalid per RFC 8259 */
            return NULL;
        }
        char c = *p;
        if (c == '\\' && *(p + 1)) {
            p++;
            if (*p == 'n') c = '\n';
            else if (*p == 'r') c = '\r';
            else if (*p == 't') c = '\t';
            else if (*p == '\\') c = '\\';
            else if (*p == '\"') c = '\"';
            else c = *p;
        }
        if (idx + 1 < max_len) {
            out_val[idx++] = c;
        }
        p++;
    }
    out_val[idx] = '\0';
    if (*p != '\"') {
        /* Unterminated string: fail closed */
        return NULL;
    }
    p++; /* consume closing quote */
    return p;
}

static NkResult nk_library_load_from_file(NkLibrary *lib, const char *target) {
    if (!lib || !target) return NK_ERROR_GENERIC;

    if (!nk_platform_file_exists(target)) {
        /* Not an error if file doesn't exist yet - library is just empty */
        return NK_OK;
    }

    FILE *f = nk_lib_fopen(target, "rb");
    if (!f) return NK_ERROR_IO;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0 || sz > 4 * 1024 * 1024) {
        fclose(f);
        return NK_ERROR_IO;
    }

    char *buf = (char *)malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NK_ERROR_OUT_OF_MEMORY;
    }

    size_t read_bytes = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[read_bytes] = '\0';

    /* Verify schema version (mandatory) */
    const char *sv = strstr(buf, "\"schema_version\"");
    if (!sv) {
        free(buf);
        return NK_ERROR_GENERIC;
    }
    sv = strchr(sv, ':');
    if (!sv) {
        free(buf);
        return NK_ERROR_GENERIC;
    }
    sv = skip_whitespace(sv + 1);
    char *sv_end = NULL;
    long version = strtol(sv, &sv_end, 10);
    if (sv_end == sv || version != 1) {
        /* Unsupported or malformed schema version: fail closed */
        free(buf);
        return NK_ERROR_GENERIC;
    }

    /* Parse games array */
    const char *p = strstr(buf, "\"games\"");
    if (!p) {
        free(buf);
        return NK_ERROR_GENERIC;
    }

    p = strchr(p, '[');
    if (!p) {
        free(buf);
        return NK_ERROR_GENERIC;
    }
    p++;

    while (*p && lib->count < NK_MAX_GAMES) {
        p = skip_whitespace(p);
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') { p++; continue; }
        p++;

        NkGameEntry entry;
        memset(&entry, 0, sizeof(entry));
        bool entry_failed = false;

        while (p && *p && *p != '}') {
            p = skip_whitespace(p);
            if (!p || *p == '}' || !*p) break;
            if (*p == ',') { p++; continue; }
            if (*p == '\"') {
                char key[64];
                const char *next_p = parse_string_val(p, key, sizeof(key));
                if (!next_p) { entry_failed = true; break; }
                p = skip_whitespace(next_p);
                if (p && *p == ':') p++;
                p = skip_whitespace(p);
                if (!p) { entry_failed = true; break; }

                if (strcmp(key, "disc_id") == 0) {
                    next_p = parse_string_val(p, entry.disc_id, sizeof(entry.disc_id));
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
                } else if (strcmp(key, "title_name") == 0) {
                    next_p = parse_string_val(p, entry.title_name, sizeof(entry.title_name));
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
                } else if (strcmp(key, "disc_version") == 0) {
                    next_p = parse_string_val(p, entry.disc_version, sizeof(entry.disc_version));
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
                } else if (strcmp(key, "iso_path") == 0) {
                    next_p = parse_string_val(p, entry.iso_path, sizeof(entry.iso_path));
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
                } else if (strcmp(key, "prepared_root") == 0) {
                    next_p = parse_string_val(p, entry.prepared_root, sizeof(entry.prepared_root));
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
                } else if (strcmp(key, "title_id") == 0) {
                    next_p = parse_string_val(p, entry.title_id, sizeof(entry.title_id));
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
                } else if (strcmp(key, "last_played") == 0) {
                    next_p = parse_string_val(p, entry.last_played, sizeof(entry.last_played));
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
                } else if (strcmp(key, "iso_size_bytes") == 0) {
                    char *endptr = NULL;
                    entry.iso_size_bytes = (uint64_t)strtoull(p, &endptr, 10);
                    if (endptr == p) { entry_failed = true; break; }
                    p = endptr;
                } else if (strcmp(key, "status") == 0) {
                    char *endptr = NULL;
                    entry.status = (NkGameSupportStatus)strtol(p, &endptr, 10);
                    if (endptr == p) { entry_failed = true; break; }
                    p = endptr;
                } else if (strcmp(key, "is_prepared") == 0) {
                    if (strncmp(p, "true", 4) == 0) {
                        entry.is_prepared = true;
                        p += 4;
                    } else if (strncmp(p, "false", 5) == 0) {
                        entry.is_prepared = false;
                        p += 5;
                    } else {
                        entry_failed = true;
                        break;
                    }
                } else {
                    /* Skip unknown value safely */
                    if (*p == '\"') {
                        char discard[256];
                        next_p = parse_string_val(p, discard, sizeof(discard));
                        if (!next_p) { entry_failed = true; break; }
                        p = next_p;
                    } else {
                        while (*p && *p != ',' && *p != '}' && !isspace((unsigned char)*p)) p++;
                    }
                }
            } else {
                p++;
            }
        }

        if (entry_failed) {
            /* A malformed member invalidates the WHOLE file, not just this
               entry. Scanning to the closing brace and continuing returned
               NK_OK with the game silently dropped, so nk_library_load never
               reached its .bak recovery and the next save made the loss
               permanent. Fail closed so the backup path actually runs. */
            free(buf);
            return NK_ERROR_GENERIC;
        }
        if (entry.disc_id[0] != '\0') {
            lib->entries[lib->count++] = entry;
        }

        if (p && *p == '}') p++;
    }

    /* Verify proper closing of games array and root object. Both delimiters
       are REQUIRED, not merely consumed when present: a file truncated right
       after a complete game object would otherwise reach the terminating NUL
       and return NK_OK, silently loading a partial library instead of falling
       through to the .bak recovery path. Truncation is exactly the case the
       backup exists for. */
    p = skip_whitespace(p);
    if (*p != ']') {
        free(buf);
        return NK_ERROR_GENERIC;
    }
    p++;
    p = skip_whitespace(p);
    if (*p != '}') {
        free(buf);
        return NK_ERROR_GENERIC;
    }
    p++;
    p = skip_whitespace(p);

    if (p && *p != '\0') {
        /* Trailing corrupt garbage after closing brace */
        free(buf);
        return NK_ERROR_GENERIC;
    }

    free(buf);
    return NK_OK;
}

NkResult nk_library_load(NkLibrary *lib, const char *file_path) {
    if (!lib) return NK_ERROR_GENERIC;
    nk_library_init(lib);

    const char *target = file_path;
    char default_path[NK_MAX_PATH];
    if (!target) {
        if (!nk_platform_get_path(NK_PATH_DATA, default_path, sizeof(default_path))) {
            return NK_ERROR_IO;
        }
        int written = snprintf(default_path + strlen(default_path), sizeof(default_path) - strlen(default_path), "%clibrary.json", nk_platform_path_separator());
        if (written < 0) return NK_ERROR_IO;
        target = default_path;
    }

    snprintf(lib->library_path, sizeof(lib->library_path), "%s", target);

    char bak_path[NK_MAX_PATH + 8];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", target);

    if (!nk_platform_file_exists(target)) {
        /* Check if backup exists even if primary is missing */
        if (nk_platform_file_exists(bak_path)) {
            NkResult bres = nk_library_load_from_file(lib, bak_path);
            if (bres == NK_OK) {
                snprintf(lib->library_path, sizeof(lib->library_path), "%s", target);
                return NK_OK;
            }
        }
        return NK_OK;
    }

    NkResult res = nk_library_load_from_file(lib, target);
    if (res != NK_OK) {
        /* Primary corrupted or invalid: attempt automatic recovery from .bak */
        if (nk_platform_file_exists(bak_path)) {
            nk_library_init(lib);
            NkResult bres = nk_library_load_from_file(lib, bak_path);
            if (bres == NK_OK) {
                snprintf(lib->library_path, sizeof(lib->library_path), "%s", target);
                return NK_OK;
            }
        }
        /* Neither the primary nor the backup loaded. A failed parse can still
           have accumulated the entries it read before giving up, and those are
           not a library a caller may act on -- saving them back would write the
           truncation to disk. Hand back an empty library with the error. */
        nk_library_init(lib);
        snprintf(lib->library_path, sizeof(lib->library_path), "%s", target);
    }

    return res;
}
