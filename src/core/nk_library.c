/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#define _POSIX_C_SOURCE 200809L

#include "nk_disc_verify.h"
#include "nk_json.h"
#include "nk_library.h"
#include "nk_platform.h"
#include "nk_user_data_schema.h"
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

/* The index of the record with disc_id and content_sha1, or -1. An empty or
   NULL content_sha1 matches the first record with disc_id. */
static int library_index_of_key(const NkLibrary *lib, const char *disc_id,
                                const char *content_sha1) {
    if (!lib || !disc_id || !*disc_id) return -1;
    bool any = !content_sha1 || !content_sha1[0];
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].disc_id, disc_id) != 0) continue;
        if (any || strcmp(lib->entries[i].content_sha1, content_sha1) == 0) return i;
    }
    return -1;
}

/* The record a disc ID alone picks out: the only one with it (unhashed_only:
   the only unhashed one), or -1 when there is none or it can't say which. */
static int library_index_of_disc_id_alone(const NkLibrary *lib, const char *disc_id,
                                          bool unhashed_only) {
    int only = -1, count = 0;
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].disc_id, disc_id) != 0) continue;
        if (unhashed_only && lib->entries[i].content_sha1[0]) continue;
        if (count++ == 0) only = i;
    }
    return count == 1 ? only : -1;
}

/* Whether a record's image file is still there. When it is, an unhashed image
   of another file is a second image (a fan translation added beside its retail
   disc), not the same disc moved. */
static bool library_image_present(const NkGameEntry *record) {
    return record->iso_path[0] && nk_platform_file_exists(record->iso_path);
}

/* The record an incoming entry replaces (nk_library.h), or -1 to add it. A
   record is replaced only when it is the same image as far as the library can
   tell; when it can't tell, both are kept, so no image's record is lost. */
static int library_index_for_entry(const NkLibrary *lib, const NkGameEntry *entry) {
    if (entry->content_sha1[0]) {
        int same = library_index_of_key(lib, entry->disc_id, entry->content_sha1);
        if (same >= 0) return same;
    }
    /* The record of the same file: this image is what that file holds now
       (re-added, or a compressed image expanded over the same path). */
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].disc_id, entry->disc_id) == 0 &&
            strcmp(lib->entries[i].iso_path, entry->iso_path) == 0) {
            return i;
        }
    }
    /* A hashed image of another file is another image, even beside a record
       written before hashing. An unhashed one replaces the only record of its
       disc ID when that record was never hashed either and its file is gone:
       the disc moved, as before hashing. A hashed record is a known image, a
       record whose file is still there is another image until hashes say
       otherwise, and beside several records the newcomer can't be told apart,
       so it is added and nothing is lost. */
    if (entry->content_sha1[0]) return -1;
    int only = library_index_of_disc_id_alone(lib, entry->disc_id, false);
    return only >= 0 && !lib->entries[only].content_sha1[0] &&
           !library_image_present(&lib->entries[only]) ? only : -1;
}

const NkGameEntry *nk_library_find_replaced(const NkLibrary *lib, const NkGameEntry *entry) {
    if (!lib || !entry || !entry->disc_id[0]) return NULL;
    int index = library_index_for_entry(lib, entry);
    return index >= 0 ? &lib->entries[index] : NULL;
}

const NkGameEntry *nk_library_find_by_disc_id(const NkLibrary *lib, const char *disc_id) {
    return nk_library_find_by_key(lib, disc_id, NULL);
}

const NkGameEntry *nk_library_find_by_key(const NkLibrary *lib, const char *disc_id,
                                          const char *content_sha1) {
    int index = library_index_of_key(lib, disc_id, content_sha1);
    return index >= 0 ? &lib->entries[index] : NULL;
}

bool nk_library_has_verified_disc_id(const NkLibrary *lib, const char *disc_id) {
    if (!lib || !disc_id || !*disc_id) return false;
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].disc_id, disc_id) == 0 &&
            lib->entries[i].disc_verification == NK_DISC_TIER_VERIFIED_RETAIL) {
            return true;
        }
    }
    return false;
}

/* A verified retail record makes its disc ID known retail: the unknown
   records with it are modified images of that disc. */
static void library_label_modified(NkLibrary *lib, const char *disc_id) {
    if (!nk_library_has_verified_disc_id(lib, disc_id)) return;
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].disc_id, disc_id) == 0 &&
            lib->entries[i].disc_verification == NK_DISC_TIER_UNKNOWN) {
            lib->entries[i].disc_verification = NK_DISC_TIER_MODIFIED;
        }
    }
}

/* One file holds one image: the other records of the disc ID that name the
   file of lib->entries[keep] are stale. That happens when a hashed image takes
   over its own record from another file (the image moved, then was added
   again): the record its new file got before it was hashed goes. */
static void library_drop_stale_records_of_file(NkLibrary *lib, int keep) {
    char disc_id[sizeof(lib->entries[0].disc_id)];
    char iso_path[sizeof(lib->entries[0].iso_path)];
    memcpy(disc_id, lib->entries[keep].disc_id, sizeof(disc_id));
    memcpy(iso_path, lib->entries[keep].iso_path, sizeof(iso_path));
    if (!iso_path[0]) return;
    int kept = 0;
    for (int i = 0; i < lib->count; i++) {
        if (i != keep && strcmp(lib->entries[i].disc_id, disc_id) == 0 &&
            strcmp(lib->entries[i].iso_path, iso_path) == 0) {
            continue;
        }
        if (kept != i) lib->entries[kept] = lib->entries[i];
        kept++;
    }
    if (kept < lib->count) {
        memset(&lib->entries[kept], 0, (size_t)(lib->count - kept) * sizeof(lib->entries[0]));
        lib->count = kept;
    }
}

/* nk_library_add_or_update, and with fresh set nk_library_add_fresh. */
static NkResult library_put(NkLibrary *lib, const NkGameEntry *entry, bool fresh) {
    if (!lib || !entry || entry->disc_id[0] == '\0') return NK_ERROR_GENERIC;

    int index = library_index_for_entry(lib, entry);
    if (index >= 0) {
        NkGameEntry *existing = &lib->entries[index];
        NkGameEntry merged = *entry;
        /* An update made without hashing, of the same file (its last-played
           time, a build's result), keeps the record's hashes and tier, which
           another writer may have just recorded. A fresh add of the file does
           not: the file may hold other bytes now, and the player checks it
           again. A size of 0 is one a writer didn't record, not another file. */
        if (!fresh && !merged.content_sha1[0] && existing->content_sha1[0] &&
            strcmp(existing->iso_path, merged.iso_path) == 0 &&
            (existing->iso_size_bytes == merged.iso_size_bytes ||
             existing->iso_size_bytes == 0 || merged.iso_size_bytes == 0)) {
            memcpy(merged.content_sha1, existing->content_sha1, sizeof(merged.content_sha1));
            memcpy(merged.content_crc32, existing->content_crc32, sizeof(merged.content_crc32));
            merged.disc_verification = existing->disc_verification;
        }
        *existing = merged;
        if (merged.content_sha1[0]) library_drop_stale_records_of_file(lib, index);
    } else {
        if (lib->count >= NK_MAX_GAMES) {
            return NK_ERROR_OUT_OF_MEMORY;
        }
        lib->entries[lib->count++] = *entry;
    }
    library_label_modified(lib, entry->disc_id);
    return NK_OK;
}

NkResult nk_library_add_or_update(NkLibrary *lib, const NkGameEntry *entry) {
    return library_put(lib, entry, false);
}

NkResult nk_library_add_fresh(NkLibrary *lib, const NkGameEntry *entry) {
    return library_put(lib, entry, true);
}

NkResult nk_library_remove(NkLibrary *lib, const char *disc_id) {
    return nk_library_remove_by_key(lib, disc_id, NULL);
}

NkResult nk_library_remove_by_file(NkLibrary *lib, const char *disc_id, const char *iso_path) {
    if (!lib || !disc_id || !*disc_id || !iso_path || !*iso_path) return NK_ERROR_GENERIC;
    for (int i = 0; i < lib->count; i++) {
        if (strcmp(lib->entries[i].disc_id, disc_id) != 0 ||
            strcmp(lib->entries[i].iso_path, iso_path) != 0) {
            continue;
        }
        for (int j = i; j < lib->count - 1; j++) lib->entries[j] = lib->entries[j + 1];
        lib->count--;
        memset(&lib->entries[lib->count], 0, sizeof(NkGameEntry));
        return NK_OK;
    }
    return NK_ERROR_FILE_NOT_FOUND;
}

NkResult nk_library_remove_by_key(NkLibrary *lib, const char *disc_id,
                                  const char *content_sha1) {
    if (!lib || !disc_id || !*disc_id) return NK_ERROR_GENERIC;

    int i;
    if (content_sha1 && content_sha1[0]) {
        i = library_index_of_key(lib, disc_id, content_sha1);
    } else {
        /* Never remove a record the disc ID alone doesn't pick out: with a
           retail disc and a modified copy, that could be the other one. An
           empty content_sha1 names the unhashed record. */
        bool unhashed_only = content_sha1 != NULL;
        i = library_index_of_disc_id_alone(lib, disc_id, unhashed_only);
        if (i < 0) {
            int candidates = 0;
            for (int j = 0; j < lib->count; j++) {
                candidates += strcmp(lib->entries[j].disc_id, disc_id) == 0 &&
                              (!unhashed_only || !lib->entries[j].content_sha1[0]);
            }
            if (candidates > 1) return NK_ERROR_AMBIGUOUS;
        }
    }
    if (i < 0) return NK_ERROR_FILE_NOT_FOUND;
    for (int j = i; j < lib->count - 1; j++) {
        lib->entries[j] = lib->entries[j + 1];
    }
    lib->count--;
    memset(&lib->entries[lib->count], 0, sizeof(NkGameEntry));
    return NK_OK;
}

/* Writes `"key": "value",` with the value escaped for JSON. Every control
 * character is escaped, so the reader (which refuses raw control characters)
 * reads back what was written. The value is streamed, so no escape ever cuts it
 * to fit a buffer: a field of quotes or control characters is written whole. */
static void write_json_string_field(FILE *f, const char *key, const char *src) {
    fprintf(f, "      \"%s\": \"", key);
    for (size_t i = 0; src && src[i]; i++) {
        unsigned char c = (unsigned char)src[i];
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

    /* A library a newer player wrote is never replaced, and an older one is kept
       as library.json.v<N>.bak first (nk_user_data_schema.h). The .bak rotation
       below copies the primary over the .bak, so a newer .bak is refused too. */
    char bak_path[NK_MAX_PATH + 8];
    snprintf(bak_path, sizeof(bak_path), "%s.bak", target);
    if (nk_user_data_file_version(bak_path, (size_t)NK_LIBRARY_MAX_BYTES) >
        NK_LIBRARY_SCHEMA_VERSION) {
        return NK_ERROR_NEWER_SCHEMA;
    }
    NkResult guard = nk_user_data_prepare_replace(target, NK_LIBRARY_SCHEMA_VERSION,
                                                  (size_t)NK_LIBRARY_MAX_BYTES, NULL);
    if (guard != NK_OK) return guard;

    char tmp_path[NK_MAX_PATH + 8];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", target);

    FILE *f = nk_lib_fopen(tmp_path, "wb");
    if (!f) return NK_ERROR_IO;

    fprintf(f, "{\n  \"schema_version\": %d,\n  \"games\": [\n", NK_LIBRARY_SCHEMA_VERSION);

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
        if (g->source_image_path[0]) {
            write_json_string_field(f, "source_image_path", g->source_image_path);
        }
        if (g->content_sha1[0]) {
            write_json_string_field(f, "content_sha1", g->content_sha1);
            write_json_string_field(f, "content_crc32", g->content_crc32);
        }
        if (g->disc_verification != NK_DISC_TIER_NOT_CHECKED) {
            write_json_string_field(f, "disc_verification",
                                    nk_disc_tier_name((NkDiscTier)g->disc_verification));
        }
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

    /* Promote the temp file over the target, keeping the previous file as .bak. */
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
        /* Copy through a temporary file and promote it only after a complete,
           verified copy. Opening bak_path directly would truncate the previous
           backup before a single byte was written, and a short write, read error
           or failed close would destroy the last recovery image while this
           function still returned NK_OK, leaving nothing to fall back to if the
           primary is later corrupted. A failed copy leaves the existing backup
           exactly as it was. */
        char bak_tmp[NK_MAX_PATH + 16];
        int bak_tmp_len = snprintf(bak_tmp, sizeof(bak_tmp), "%s.new", bak_path);
        if (bak_tmp_len > 0 && (size_t)bak_tmp_len < sizeof(bak_tmp)) {
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

/* library.json is read by scanning the fixed layout nk_library_save writes, not
 * through an nk_json tree; these are its scanner helpers. */
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

/* Reads the literal `true` or `false` at p. Returns the text after it, or NULL
 * when neither is there. */
static const char *parse_bool_val(const char *p, bool *out_value) {
    if (strncmp(p, "true", 4) == 0) {
        *out_value = true;
        return p + 4;
    }
    if (strncmp(p, "false", 5) == 0) {
        *out_value = false;
        return p + 5;
    }
    return NULL;
}

/* Exactly `digits` lowercase or uppercase hex digits, stored lowercase. */
static bool library_hex_field(char *value, size_t digits) {
    if (strlen(value) != digits) return false;
    for (size_t i = 0; i < digits; i++) {
        char c = value[i];
        if (c >= 'A' && c <= 'F') value[i] = (char)(c - 'A' + 'a');
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
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
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (file_size <= 0 || file_size > NK_LIBRARY_MAX_BYTES) {
        fclose(f);
        return NK_ERROR_IO;
    }

    char *buf = (char *)malloc((size_t)file_size + 1);
    if (!buf) {
        fclose(f);
        return NK_ERROR_OUT_OF_MEMORY;
    }

    size_t read_bytes = fread(buf, 1, (size_t)file_size, f);
    fclose(f);
    buf[read_bytes] = '\0';

    /* The schema version is mandatory; an unknown one is refused, not guessed at.
       A version below this build's would be migrated forward here one step at a
       time (none exists yet: version 1 is the first); a newer one is refused by
       name so nk_library_load neither falls back to an older .bak nor lets a
       save replace it. */
    const char *schema_pos = strstr(buf, "\"schema_version\"");
    if (!schema_pos) {
        free(buf);
        return NK_ERROR_GENERIC;
    }
    schema_pos = strchr(schema_pos, ':');
    if (!schema_pos) {
        free(buf);
        return NK_ERROR_GENERIC;
    }
    schema_pos = skip_whitespace(schema_pos + 1);
    char *schema_end = NULL;
    long version = strtol(schema_pos, &schema_end, 10);
    if (schema_end != schema_pos && version > NK_LIBRARY_SCHEMA_VERSION) {
        free(buf);
        lib->newer_schema_version = version;
        return NK_ERROR_NEWER_SCHEMA;
    }
    if (schema_end == schema_pos || version != NK_LIBRARY_SCHEMA_VERSION) {
        /* Malformed or pre-version-1 schema: fail closed so the .bak is tried. */
        free(buf);
        return NK_ERROR_GENERIC;
    }

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
                } else if (strcmp(key, "source_image_path") == 0) {
                    next_p = parse_string_val(p, entry.source_image_path,
                                              sizeof(entry.source_image_path));
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
                } else if (strcmp(key, "content_sha1") == 0 ||
                           strcmp(key, "content_crc32") == 0) {
                    bool sha1 = strcmp(key, "content_sha1") == 0;
                    char *field = sha1 ? entry.content_sha1 : entry.content_crc32;
                    size_t field_size = sha1 ? sizeof(entry.content_sha1)
                                             : sizeof(entry.content_crc32);
                    char value[64];
                    next_p = parse_string_val(p, value, sizeof(value));
                    if (!next_p || !library_hex_field(value, field_size - 1u)) {
                        entry_failed = true;
                        break;
                    }
                    memcpy(field, value, field_size);
                    p = next_p;
                } else if (strcmp(key, "disc_verification") == 0) {
                    /* A tier name this build does not know reads as not
                       checked, so the next check fills it in again. */
                    char name[32];
                    NkDiscTier tier;
                    next_p = parse_string_val(p, name, sizeof(name));
                    if (!next_p) { entry_failed = true; break; }
                    (void)nk_disc_tier_from_name(name, &tier);
                    entry.disc_verification = (uint32_t)tier;
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
                    next_p = parse_bool_val(p, &entry.is_prepared);
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
                } else if (strcmp(key, "is_experimental") == 0 ||
                           strcmp(key, "executable_boot_fallback") == 0) {
                    bool value;
                    next_p = parse_bool_val(p, &value);
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
                    if (strcmp(key, "is_experimental") == 0) {
                        entry.is_experimental = value;
                    } else {
                        entry.executable_boot_fallback = value;
                    }
                } else if (strcmp(key, "assets_staged") == 0) {
                    next_p = parse_bool_val(p, &entry.assets_staged);
                    if (!next_p) { entry_failed = true; break; }
                    p = next_p;
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

        if (!entry_failed && (entry.content_sha1[0] != '\0') != (entry.content_crc32[0] != '\0')) {
            entry_failed = true; /* one hash without the other is not a record we wrote */
        }
        if (entry_failed) {
            /* A malformed member invalidates the WHOLE file, not just this
               entry. Skipping to the closing brace and continuing would return
               NK_OK with the game silently dropped, so nk_library_load would
               never reach its .bak recovery and the next save would make the
               loss permanent. Fail closed so the backup path runs. */
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
            if (bres == NK_ERROR_NEWER_SCHEMA) {
                long newer = lib->newer_schema_version;
                nk_library_init(lib);
                snprintf(lib->library_path, sizeof(lib->library_path), "%s", target);
                lib->newer_schema_version = newer;
                return bres;
            }
        }
        return NK_OK;
    }

    NkResult res = nk_library_load_from_file(lib, target);
    /* A newer player wrote the primary: the .bak is older, and loading it would
       let the next save replace the newer library. It is refused by name. */
    if (res != NK_OK && res != NK_ERROR_NEWER_SCHEMA) {
        /* Primary corrupted or invalid: attempt automatic recovery from .bak */
        if (nk_platform_file_exists(bak_path)) {
            nk_library_init(lib);
            NkResult bres = nk_library_load_from_file(lib, bak_path);
            if (bres == NK_OK) {
                snprintf(lib->library_path, sizeof(lib->library_path), "%s", target);
                return NK_OK;
            }
            if (bres == NK_ERROR_NEWER_SCHEMA) res = bres;
        }
    }
    if (res != NK_OK) {
        /* Neither the primary nor the backup loaded. A failed parse can still
           have accumulated the entries it read before giving up, and those are
           not a library a caller may act on -- saving them back would write the
           truncation to disk. Hand back an empty library with the error. */
        long newer = lib->newer_schema_version;
        nk_library_init(lib);
        snprintf(lib->library_path, sizeof(lib->library_path), "%s", target);
        if (res == NK_ERROR_NEWER_SCHEMA) lib->newer_schema_version = newer;
    }

    return res;
}

/* locks/library.lock beside the library file, as tools/nk_cli.py names it. */
static bool library_lock_paths(const char *library_path, char *dir, size_t dir_len,
                               char *file, size_t file_len) {
    const char *slash = NULL;
    for (const char *p = library_path; *p; p++) {
        if (*p == '/' || *p == '\\') slash = p;
    }
    char sep = nk_platform_path_separator();
    int n = slash ? snprintf(dir, dir_len, "%.*s%clocks", (int)(slash - library_path),
                             library_path, sep)
                  : snprintf(dir, dir_len, "locks");
    if (n < 0 || (size_t)n >= dir_len) return false;
    n = snprintf(file, file_len, "%s%clibrary.lock", dir, sep);
    return n >= 0 && (size_t)n < file_len;
}

NkResult nk_library_lock_acquire(NkFileLock *lock, const char *library_path,
                                 unsigned timeout_ms) {
    if (!lock) return NK_ERROR_GENERIC;
    memset(lock, 0, sizeof(*lock));
    if (!library_path || !*library_path) return NK_ERROR_GENERIC;
    char dir[NK_MAX_PATH];
    char path[NK_MAX_PATH + 16];
    if (!library_lock_paths(library_path, dir, sizeof(dir), path, sizeof(path))) {
        return NK_ERROR_PATH_TOO_LONG;
    }
    if (!nk_platform_mkdir_p_private(dir)) return NK_ERROR_IO;
    return nk_platform_file_lock_acquire(lock, path, timeout_ms) ? NK_OK : NK_ERROR_IO;
}

/* lib takes the saved records: its own keep their order, as saved where the file
   has them, then come the ones another writer added. An own record the file does
   not have (a sample, or one an earlier save could not write) stays, except the
   removed one. view is scratch space of the same size. */
static void library_adopt(NkLibrary *lib, const NkLibrary *saved, const char *removed,
                          const char *removed_sha1, const char *removed_file,
                          NkLibrary *view, bool *taken) {
    view->count = 0;
    memset(taken, 0, (size_t)NK_MAX_GAMES * sizeof(*taken));
    bool removed_any_image = !removed_sha1 || !removed_sha1[0];
    for (int i = 0; i < lib->count && view->count < NK_MAX_GAMES; i++) {
        const NkGameEntry *own = &lib->entries[i];
        /* Its record in the file, by the key add_or_update uses; each record
           in the file is taken by one own record at most. */
        int on_disk = own->is_sample ? -1 : library_index_for_entry(saved, own);
        if (on_disk >= 0 && taken[on_disk]) on_disk = -1;
        /* The removed record: gone from the file, and named by the removal. */
        if (on_disk < 0 && removed && !own->is_sample && strcmp(own->disc_id, removed) == 0 &&
            (removed_file ? strcmp(own->iso_path, removed_file) == 0
                          : removed_any_image || strcmp(own->content_sha1, removed_sha1) == 0)) {
            continue;
        }
        if (on_disk >= 0) taken[on_disk] = true;
        view->entries[view->count++] = on_disk >= 0 ? saved->entries[on_disk] : *own;
    }
    for (int i = 0; i < saved->count && view->count < NK_MAX_GAMES; i++) {
        if (!taken[i]) view->entries[view->count++] = saved->entries[i];
    }
    int previous = lib->count;
    memcpy(lib->entries, view->entries, (size_t)view->count * sizeof(lib->entries[0]));
    if (previous > view->count) {
        memset(&lib->entries[view->count], 0,
               (size_t)(previous - view->count) * sizeof(lib->entries[0]));
    }
    lib->count = view->count;
}

/* Puts entry (as a fresh add when fresh is set), or removes disc_id (the record
   of iso_path when it is given, else the one with content_sha1) when entry is
   NULL, in the file under the lock. */
static NkResult library_commit(NkLibrary *lib, const NkGameEntry *entry, bool fresh,
                               const char *disc_id, const char *content_sha1,
                               const char *iso_path, const char *file_path) {
    char target[NK_MAX_PATH];
    if (file_path) {
        snprintf(target, sizeof(target), "%s", file_path);
    } else if (lib->library_path[0]) {
        snprintf(target, sizeof(target), "%s", lib->library_path);
    } else {
        if (!nk_platform_get_path(NK_PATH_DATA, target, sizeof(target))) return NK_ERROR_IO;
        size_t used = strlen(target);
        int written = snprintf(target + used, sizeof(target) - used, "%clibrary.json",
                               nk_platform_path_separator());
        if (written < 0 || (size_t)written >= sizeof(target) - used) return NK_ERROR_PATH_TOO_LONG;
    }

    NkLibrary *disk = (NkLibrary *)malloc(sizeof(*disk));
    NkLibrary *view = (NkLibrary *)malloc(sizeof(*view));
    bool *taken = (bool *)malloc((size_t)NK_MAX_GAMES * sizeof(*taken));
    if (!disk || !view || !taken) {
        free(disk);
        free(view);
        free(taken);
        return NK_ERROR_OUT_OF_MEMORY;
    }
    NkFileLock lock;
    NkResult res = nk_library_lock_acquire(&lock, target, NK_LIBRARY_LOCK_TIMEOUT_MS);
    if (res == NK_OK) {
        res = nk_library_load(disk, target);
        if (res == NK_ERROR_NEWER_SCHEMA) {
            lib->newer_schema_version = disk->newer_schema_version;
        } else {
            if (res != NK_OK) {
                /* Neither the file nor its .bak reads: nothing on disk can be kept,
                   so lib is the library, as nk_library_save would write it. */
                memcpy(disk->entries, lib->entries,
                       (size_t)lib->count * sizeof(lib->entries[0]));
                disk->count = lib->count;
            }
            res = entry ? library_put(disk, entry, fresh)
                  : iso_path ? nk_library_remove_by_file(disk, disc_id, iso_path)
                             : nk_library_remove_by_key(disk, disc_id, content_sha1);
            if (res == NK_OK) {
                res = nk_library_save(disk, target);
            } else if (!entry && res == NK_ERROR_FILE_NOT_FOUND) {
                res = NK_OK;  /* another writer removed it already */
            }
            if (res == NK_OK) {
                library_adopt(lib, disk, disc_id, content_sha1, iso_path, view, taken);
            }
        }
        nk_platform_file_lock_release(&lock);
    }
    free(taken);
    free(view);
    free(disk);
    return res;
}

NkResult nk_library_commit_entry(NkLibrary *lib, const NkGameEntry *entry,
                                 const char *file_path) {
    if (!lib || !entry || entry->disc_id[0] == '\0') return NK_ERROR_GENERIC;
    return library_commit(lib, entry, false, NULL, NULL, NULL, file_path);
}

NkResult nk_library_commit_fresh(NkLibrary *lib, const NkGameEntry *entry,
                                 const char *file_path) {
    if (!lib || !entry || entry->disc_id[0] == '\0') return NK_ERROR_GENERIC;
    return library_commit(lib, entry, true, NULL, NULL, NULL, file_path);
}

NkResult nk_library_commit_remove(NkLibrary *lib, const char *disc_id,
                                  const char *file_path) {
    return nk_library_commit_remove_by_key(lib, disc_id, NULL, file_path);
}

NkResult nk_library_commit_remove_by_key(NkLibrary *lib, const char *disc_id,
                                         const char *content_sha1, const char *file_path) {
    if (!lib || !disc_id || !*disc_id) return NK_ERROR_GENERIC;
    return library_commit(lib, NULL, false, disc_id, content_sha1, NULL, file_path);
}

NkResult nk_library_commit_remove_by_file(NkLibrary *lib, const char *disc_id,
                                          const char *iso_path, const char *file_path) {
    if (!lib || !disc_id || !*disc_id || !iso_path || !*iso_path) return NK_ERROR_GENERIC;
    return library_commit(lib, NULL, false, disc_id, NULL, iso_path, file_path);
}
