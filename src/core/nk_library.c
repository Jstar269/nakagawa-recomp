/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#define _POSIX_C_SOURCE 200809L

#include "nk_json.h"
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
    if (!path || !mode) return NULL;
    WCHAR wpath[32768];
    WCHAR wmode[32];
    /* Strict conversion: invalid UTF-8 must fail rather than open a path in
       which the bad bytes became U+FFFD. */
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1,
                            wpath, (int)(sizeof(wpath) / sizeof(wpath[0]))) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, mode, -1,
                            wmode, (int)(sizeof(wmode) / sizeof(wmode[0]))) <= 0) {
        return NULL;
    }
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

/* Writes `"key": "value",` with the value escaped for JSON. Every control
 * character is escaped, so the reader (which refuses raw control characters)
 * reads back what was written. The value is streamed, so no escape ever cuts it
 * to fit a buffer: a field of quotes or control characters is written whole. */
static void write_json_string_field(FILE *f, const char *key, const char *src) {
    fprintf(f, "      \"%s\": \"", key);
    for (size_t s = 0; src && src[s]; s++) {
        unsigned char c = (unsigned char)src[s];
        if (c == '\"' || c == '\\') {
            fputc('\\', f);
            fputc((int)c, f);
        } else if (c == '\n') {
            fputs("\\n", f);
        } else if (c == '\r') {
            fputs("\\r", f);
        } else if (c == '\t') {
            fputs("\\t", f);
        } else if (c < 0x20) {
            fprintf(f, "\\u%04x", (unsigned)c);
        } else {
            fputc((int)c, f);
        }
    }
    fputs("\",\n", f);
}

NkResult nk_library_save(const NkLibrary *lib, const char *file_path) {
    if (!lib) return NK_ERROR_GENERIC;
    if (lib->count < 0 || lib->count > NK_MAX_GAMES) return NK_ERROR_GENERIC;
    for (int i = 0; i < lib->count; i++) {
        if (!memchr(lib->entries[i].boot_executable, '\0',
                    sizeof(lib->entries[i].boot_executable))) {
            return NK_ERROR_INVALID_EXECUTABLE;
        }
    }

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

    /* Bundled samples are in memory only and are never written. */
    int persisted_total = 0;
    for (int i = 0; i < lib->count; i++) {
        if (!lib->entries[i].is_sample) persisted_total++;
    }
    int persisted_index = 0;
    for (int i = 0; i < lib->count; i++) {
        const NkGameEntry *g = &lib->entries[i];
        if (g->is_sample) continue;
        fprintf(f, "    {\n");
        write_json_string_field(f, "disc_id", g->disc_id);
        write_json_string_field(f, "title_name", g->title_name);
        write_json_string_field(f, "disc_version", g->disc_version);
        write_json_string_field(f, "iso_path", g->iso_path);
        write_json_string_field(f, "prepared_root", g->prepared_root);
        write_json_string_field(f, "title_id", g->title_id);
        fprintf(f, "      \"iso_size_bytes\": %llu,\n", (unsigned long long)g->iso_size_bytes);
        fprintf(f, "      \"status\": %d,\n", (int)g->status);
        fprintf(f, "      \"is_experimental\": %s,\n", g->is_experimental ? "true" : "false");
        fprintf(f, "      \"executable_eboot_kind\": %u,\n", (unsigned)g->executable_eboot_kind);
        fprintf(f, "      \"executable_boot_kind\": %u,\n", (unsigned)g->executable_boot_kind);
        fprintf(f, "      \"executable_selection\": %u,\n", (unsigned)g->executable_selection);
        fprintf(f, "      \"executable_boot_fallback\": %s,\n",
                g->executable_boot_fallback ? "true" : "false");
        write_json_string_field(f, "boot_executable", g->boot_executable);
        write_json_string_field(f, "selected_executable", g->selected_executable);
        fprintf(f, "      \"is_prepared\": %s,\n", g->is_prepared ? "true" : "false");
        fprintf(f, "      \"assets_staged\": %s,\n", g->assets_staged ? "true" : "false");
        fprintf(f, "      \"extracted_asset_count\": %u,\n", (unsigned)g->extracted_asset_count);
        fprintf(f, "      \"extracted_audio_count\": %u,\n", (unsigned)g->extracted_audio_count);
        fprintf(f, "      \"extracted_visual_count\": %u,\n", (unsigned)g->extracted_visual_count);
        fprintf(f, "      \"extracted_layout_count\": %u,\n", (unsigned)g->extracted_layout_count);
        fprintf(f, "      \"last_played\": \"%s\"\n", g->last_played);
        fprintf(f, "    }%s\n", (persisted_index + 1 < persisted_total) ? "," : "");
        persisted_index++;
    }

    fprintf(f, "  ]\n}\n");
    if (fflush(f) != 0) {
        fclose(f);
#if defined(_WIN32) || defined(_WIN64)
        WCHAR wtmp[32768];
        if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, tmp_path, -1, wtmp, 32768) > 0) {
            DeleteFileW(wtmp);
        }
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
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, tmp_path, -1, wtmp, 32768) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, target, -1, wtarget, 32768) <= 0 ||
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bak_path, -1, wbak, 32768) <= 0) {
        return NK_ERROR_IO;
    }

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

/* Bounded JSON string reader. Escapes are decoded by the same reader the
 * manifest and profile parsers use (nk_json_decode_escape), so \uXXXX, surrogate
 * pairs and the short escapes all produce their UTF-8 bytes. A malformed escape,
 * a raw control character, malformed UTF-8 or an unterminated string fails the
 * parse. A code point that does not fit the field is dropped whole, so a value
 * is never cut inside a UTF-8 sequence. */
static const char *parse_string_val(const char *p, char *out_val, size_t max_len) {
    if (!out_val || max_len == 0) return NULL;
    out_val[0] = '\0';
    p = skip_whitespace(p);
    if (!p || *p != '\"') return NULL;
    p++;
    size_t idx = 0;
    while (*p && *p != '\"') {
        unsigned char c = (unsigned char)*p;
        char encoded[4];
        size_t length = 0;
        if (c < 0x20) {
            /* Raw control characters are invalid in a JSON string (RFC 8259). */
            return NULL;
        } else if (c == '\\') {
            /* An escape reads at most 11 bytes after the backslash (a surrogate pair). */
            size_t avail = 0;
            while (avail < 11 && p[1 + avail] != '\0') avail++;
            uint32_t cp = 0;
            size_t used = 0;
            if (nk_json_decode_escape(p + 1, avail, &cp, &used) != NULL) return NULL;
            length = nk_json_utf8_encode(cp, encoded);
            p += 1 + used;
        } else if (c < 0x80) {
            encoded[0] = (char)c;
            length = 1;
            p++;
        } else {
            /* Raw non-ASCII: copy one complete, validated UTF-8 sequence. */
            size_t seq = (c >= 0xC2 && c <= 0xDF) ? 2u
                       : (c >= 0xE0 && c <= 0xEF) ? 3u
                       : (c >= 0xF0 && c <= 0xF4) ? 4u
                       : 0u;
            if (seq == 0 || !nk_json_validate_utf8((const uint8_t *)p, seq)) return NULL;
            memcpy(encoded, p, seq);
            length = seq;
            p += seq;
        }
        if (idx + length < max_len) {
            memcpy(out_val + idx, encoded, length);
            idx += length;
        }
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

    while (p && *p && lib->count < NK_MAX_GAMES) {
        p = skip_whitespace(p);
        if (!p || !*p) break;
        if (*p == ']') break;
        if (*p == ',') { p++; continue; }
        if (*p != '{') {
            free(buf);
            return NK_ERROR_GENERIC;
        }
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
                } else if (strcmp(key, "boot_executable") == 0) {
                    char parsed_boot_executable[NK_MAX_EXECUTABLE_PATH + 1u];
                    next_p = parse_string_val(
                        p, parsed_boot_executable, sizeof(parsed_boot_executable)
                    );
                    if (!next_p) { entry_failed = true; break; }
                    size_t boot_length = strlen(parsed_boot_executable);
                    if (boot_length >= sizeof(entry.boot_executable)) {
                        free(buf);
                        return NK_ERROR_INVALID_EXECUTABLE;
                    }
                    memcpy(entry.boot_executable, parsed_boot_executable,
                           boot_length + 1u);
                    p = next_p;
                } else if (strcmp(key, "selected_executable") == 0) {
                    next_p = parse_string_val(p, entry.selected_executable,
                                              sizeof(entry.selected_executable));
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
                } else if (strcmp(key, "is_experimental") == 0 ||
                           strcmp(key, "executable_boot_fallback") == 0) {
                    bool value;
                    if (strncmp(p, "true", 4) == 0) {
                        value = true;
                        p += 4;
                    } else if (strncmp(p, "false", 5) == 0) {
                        value = false;
                        p += 5;
                    } else {
                        entry_failed = true;
                        break;
                    }
                    if (strcmp(key, "is_experimental") == 0) {
                        entry.is_experimental = value;
                    } else {
                        entry.executable_boot_fallback = value;
                    }
                } else if (strcmp(key, "assets_staged") == 0) {
                    if (strncmp(p, "true", 4) == 0) {
                        entry.assets_staged = true;
                        p += 4;
                    } else if (strncmp(p, "false", 5) == 0) {
                        entry.assets_staged = false;
                        p += 5;
                    } else {
                        entry_failed = true;
                        break;
                    }
                } else if (strcmp(key, "extracted_asset_count") == 0 ||
                           strcmp(key, "extracted_audio_count") == 0 ||
                           strcmp(key, "extracted_visual_count") == 0 ||
                           strcmp(key, "extracted_layout_count") == 0 ||
                           strcmp(key, "executable_eboot_kind") == 0 ||
                           strcmp(key, "executable_boot_kind") == 0 ||
                           strcmp(key, "executable_selection") == 0) {
                    char *endptr = NULL;
                    unsigned long value = strtoul(p, &endptr, 10);
                    unsigned long maximum = UINT32_MAX;
                    if (strcmp(key, "executable_eboot_kind") == 0 ||
                        strcmp(key, "executable_boot_kind") == 0) maximum = 5;
                    if (strcmp(key, "executable_selection") == 0) maximum = 2;
                    if (endptr == p || value > maximum) {
                        entry_failed = true;
                        break;
                    }
                    if (strcmp(key, "executable_eboot_kind") == 0) {
                        entry.executable_eboot_kind = (uint32_t)value;
                    } else if (strcmp(key, "executable_boot_kind") == 0) {
                        entry.executable_boot_kind = (uint32_t)value;
                    } else if (strcmp(key, "executable_selection") == 0) {
                        entry.executable_selection = (uint32_t)value;
                    } else if (strcmp(key, "extracted_asset_count") == 0) {
                        entry.extracted_asset_count = (uint32_t)value;
                    } else if (strcmp(key, "extracted_audio_count") == 0) {
                        entry.extracted_audio_count = (uint32_t)value;
                    } else if (strcmp(key, "extracted_visual_count") == 0) {
                        entry.extracted_visual_count = (uint32_t)value;
                    } else {
                        entry.extracted_layout_count = (uint32_t)value;
                    }
                    p = endptr;
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
                entry_failed = true;
                break;
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
