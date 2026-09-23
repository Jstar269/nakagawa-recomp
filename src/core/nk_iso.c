/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "nk_iso.h"
#ifndef NK_ISO_NO_PLAYER_EXTRAS
#include "nk_platform.h"
#endif
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_MSC_VER)
#include <strings.h>
#endif

#if defined(_WIN32) || defined(_WIN64)
#include <windows.h>
#endif

#if defined(_MSC_VER)
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#endif

#define SECTOR_SIZE 2048
#define PVD_SECTOR 16
#define NK_ISO_MAX_DEPTH 16
/* Upper bound on a directory extent we are willing to buffer. A recorded size
   above this is treated as unusable and replaced by a conservative window.
   Both readers must agree: two different limits for the same field mean the
   inspect and extract paths disagree about what a valid image looks like. */
#define NK_ISO_MAX_DIR_BYTES (512u * 1024u)
#define NK_ISO_MAX_EXECUTABLE_BYTES (512u * 1024u * 1024u)
#define NK_ISO_MAX_ELF_PROGRAM_HEADERS 128u

/* SFO Header Magic: \x00PSF */
static const uint8_t SFO_MAGIC[4] = { 0x00, 'P', 'S', 'F' };

/* Safe helper to read uint32 little-endian */
static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Safe helper to read uint16 little-endian */
static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

/* Safe helper to read uint32 big-endian */
static inline uint32_t read_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static FILE *nk_iso_fopen(const char *path, const char *mode) {
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

static inline int nk_iso_fseek64(FILE *f, int64_t offset, int whence) {
#if defined(_WIN32) || defined(_WIN64)
    return _fseeki64(f, offset, whence);
#else
    return fseeko(f, (off_t)offset, whence);
#endif
}

static inline int64_t nk_iso_ftell64(FILE *f) {
#if defined(_WIN32) || defined(_WIN64)
    return _ftelli64(f);
#else
    return (int64_t)ftello(f);
#endif
}

struct NkIsoReader {
    FILE *f;
    uint64_t file_size;
    uint32_t root_lba;
    uint32_t root_size;
    char volume_id[33];
};

const char *nk_iso_reader_volume_id(const NkIsoReader *reader) {
    return reader ? reader->volume_id : "";
}

uint64_t nk_iso_reader_file_size(const NkIsoReader *reader) {
    return reader ? reader->file_size : 0;
}

NkIsoReader *nk_iso_reader_open(const char *iso_path) {
    if (!iso_path || !iso_path[0]) return NULL;

    FILE *f = nk_iso_fopen(iso_path, "rb");
    if (!f) return NULL;

    if (nk_iso_fseek64(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    int64_t raw_sz = nk_iso_ftell64(f);
    if (raw_sz <= 0 || (uint64_t)raw_sz < (uint64_t)(PVD_SECTOR + 1) * SECTOR_SIZE) {
        fclose(f);
        return NULL;
    }
    uint64_t file_size = (uint64_t)raw_sz;

    uint8_t pvd[SECTOR_SIZE];
    if (nk_iso_fseek64(f, (int64_t)PVD_SECTOR * SECTOR_SIZE, SEEK_SET) != 0 ||
        fread(pvd, 1, SECTOR_SIZE, f) != SECTOR_SIZE) {
        fclose(f);
        return NULL;
    }

    if (pvd[0] != 0x01 || memcmp(&pvd[1], "CD001", 5) != 0) {
        fclose(f);
        return NULL;
    }

    uint32_t root_lba_le = read_le32(&pvd[158]);
    uint32_t root_lba_be = read_be32(&pvd[162]);
    uint32_t root_sz_le = read_le32(&pvd[166]);
    uint32_t root_sz_be = read_be32(&pvd[170]);

    if (root_lba_le != root_lba_be || root_sz_le != root_sz_be) {
        fclose(f);
        return NULL;
    }

    uint64_t root_offset = (uint64_t)root_lba_le * SECTOR_SIZE;
    if (root_offset > file_size || (uint64_t)root_sz_le > file_size - root_offset ||
        root_sz_le == 0 || root_sz_le > NK_ISO_MAX_DIR_BYTES) {
        fclose(f);
        return NULL;
    }

    NkIsoReader *reader = (NkIsoReader *)calloc(1, sizeof(NkIsoReader));
    if (!reader) {
        fclose(f);
        return NULL;
    }

    reader->f = f;
    reader->file_size = file_size;
    reader->root_lba = root_lba_le;
    reader->root_size = root_sz_le;

    memcpy(reader->volume_id, &pvd[40], 32);
    reader->volume_id[32] = '\0';
    for (int i = 31; i >= 0; i--) {
        if (reader->volume_id[i] == ' ' || reader->volume_id[i] == '\0') {
            reader->volume_id[i] = '\0';
        } else {
            break;
        }
    }

    return reader;
}

void nk_iso_reader_close(NkIsoReader *reader) {
    if (!reader) return;
    if (reader->f) {
        fclose(reader->f);
        reader->f = NULL;
    }
    free(reader);
}

int nk_iso_reader_read(NkIsoReader *reader, uint32_t lba, uint64_t offset, void *dst, uint32_t bytes) {
    if (!reader || !reader->f || !dst) return -1;
    if (bytes == 0) return 0;

    uint64_t file_pos = ((uint64_t)lba * SECTOR_SIZE) + offset;
    if (file_pos >= reader->file_size) {
        return 0; /* EOF */
    }

    uint64_t available = reader->file_size - file_pos;
    if ((uint64_t)bytes > available) {
        bytes = (uint32_t)available;
    }

    if (nk_iso_fseek64(reader->f, (int64_t)file_pos, SEEK_SET) != 0) {
        return -1;
    }

    size_t got = fread(dst, 1, bytes, reader->f);
    return (int)got;
}

static bool nk_iso_name_matches(const char *token, const uint8_t *name_bytes, uint8_t name_len) {
    if (!token || !name_bytes || name_len == 0) return false;

    size_t comp_len = name_len;
    for (size_t i = 0; i < name_len; i++) {
        if (name_bytes[i] == ';') {
            comp_len = i;
            break;
        }
    }

    size_t tok_len = strlen(token);
    for (size_t i = 0; i < tok_len; i++) {
        if (token[i] == ';') {
            tok_len = i;
            break;
        }
    }

    if (tok_len == comp_len && strncasecmp(token, (const char *)name_bytes, comp_len) == 0) {
        return true;
    }

    if (comp_len == tok_len + 1 && name_bytes[tok_len] == '.' &&
        strncasecmp(token, (const char *)name_bytes, tok_len) == 0) {
        return true;
    }

    if (tok_len == comp_len + 1 && token[comp_len] == '.' &&
        strncasecmp(token, (const char *)name_bytes, comp_len) == 0) {
        return true;
    }

    return false;
}

static bool nk_iso_find_in_dir(NkIsoReader *reader, uint32_t dir_lba, uint32_t dir_size,
                               const char *token, uint32_t *out_lba, uint32_t *out_size,
                               bool *out_is_dir) {
    if (!reader || !reader->f || dir_size == 0 || dir_size > NK_ISO_MAX_DIR_BYTES) return false;

    uint64_t offset = (uint64_t)dir_lba * SECTOR_SIZE;
    if (offset > reader->file_size || (uint64_t)dir_size > reader->file_size - offset) {
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc(dir_size);
    if (!buf) return false;

    if (nk_iso_fseek64(reader->f, (int64_t)offset, SEEK_SET) != 0 ||
        fread(buf, 1, dir_size, reader->f) != dir_size) {
        free(buf);
        return false;
    }

    bool found = false;
    size_t pos = 0;
    while (pos < dir_size) {
        uint8_t rec_len = buf[pos];
        if (rec_len == 0) {
            pos = ((pos / SECTOR_SIZE) + 1u) * SECTOR_SIZE;
            continue;
        }
        if (rec_len < 34u || rec_len > dir_size - pos || (pos % SECTOR_SIZE) + rec_len > SECTOR_SIZE) {
            break;
        }
        uint8_t name_len = buf[pos + 32];
        if (name_len == 0 || 33u + name_len > rec_len) {
            break;
        }

        /* Skip '.' (0) and '..' (1) */
        const uint8_t *name_bytes = &buf[pos + 33];
        if (name_len == 1 && (name_bytes[0] == 0 || name_bytes[0] == 1)) {
            pos += rec_len;
            continue;
        }

        uint32_t ext_lba_le = read_le32(&buf[pos + 2]);
        uint32_t ext_lba_be = read_be32(&buf[pos + 6]);
        uint32_t ext_sz_le = read_le32(&buf[pos + 10]);
        uint32_t ext_sz_be = read_be32(&buf[pos + 14]);

        if (ext_lba_le != ext_lba_be || ext_sz_le != ext_sz_be) {
            pos += rec_len;
            continue;
        }

        uint64_t ext_off = (uint64_t)ext_lba_le * SECTOR_SIZE;
        if (ext_off > reader->file_size || (uint64_t)ext_sz_le > reader->file_size - ext_off) {
            pos += rec_len;
            continue;
        }

        if (nk_iso_name_matches(token, name_bytes, name_len)) {
            if (out_lba) *out_lba = ext_lba_le;
            if (out_size) *out_size = ext_sz_le;
            if (out_is_dir) *out_is_dir = (buf[pos + 25] & 0x02u) != 0;
            found = true;
            break;
        }

        pos += rec_len;
    }

    free(buf);
    return found;
}

int nk_iso_reader_lookup(NkIsoReader *reader, const char *path, uint32_t *out_lba, uint32_t *out_size, bool *out_is_dir) {
    if (!reader || !path) return -1;

    while (*path == '/' || *path == '\\') path++;

    if (*path == '\0') {
        if (out_lba) *out_lba = reader->root_lba;
        if (out_size) *out_size = reader->root_size;
        if (out_is_dir) *out_is_dir = true;
        return 0;
    }

    char path_copy[512];
    snprintf(path_copy, sizeof(path_copy), "%s", path);

    char *saveptr = NULL;
    char *token = strtok_r(path_copy, "/\\", &saveptr);
    if (!token) {
        if (out_lba) *out_lba = reader->root_lba;
        if (out_size) *out_size = reader->root_size;
        if (out_is_dir) *out_is_dir = true;
        return 0;
    }

    uint32_t cur_lba = reader->root_lba;
    uint32_t cur_size = reader->root_size;
    int depth = 0;

    while (token != NULL) {
        if (++depth > NK_ISO_MAX_DEPTH) {
            return -1;
        }

        if (strcmp(token, ".") == 0) {
            token = strtok_r(NULL, "/\\", &saveptr);
            continue;
        }
        if (strcmp(token, "..") == 0) {
            return -1;
        }

        char *next_token = strtok_r(NULL, "/\\", &saveptr);
        bool is_last = (next_token == NULL);

        uint32_t entry_lba = 0;
        uint32_t entry_size = 0;
        bool entry_is_dir = false;

        if (!nk_iso_find_in_dir(reader, cur_lba, cur_size, token, &entry_lba, &entry_size, &entry_is_dir)) {
            return -1;
        }

        if (is_last) {
            if (out_lba) *out_lba = entry_lba;
            if (out_size) *out_size = entry_size;
            if (out_is_dir) *out_is_dir = entry_is_dir;
            return 0;
        }

        if (!entry_is_dir) {
            return -1;
        }

        cur_lba = entry_lba;
        cur_size = entry_size;
        token = next_token;
    }

    return -1;
}

int nk_iso_reader_list(NkIsoReader *reader, const char *dir_path, uint32_t index, NkIsoDirEntry *out_entry) {
    if (!reader || !reader->f || !out_entry) return -1;

    uint32_t dir_lba = reader->root_lba;
    uint32_t dir_size = reader->root_size;

    if (dir_path) {
        while (*dir_path == '/' || *dir_path == '\\') dir_path++;
        if (*dir_path != '\0') {
            bool is_dir = false;
            if (nk_iso_reader_lookup(reader, dir_path, &dir_lba, &dir_size, &is_dir) != 0 || !is_dir) {
                return -1;
            }
        }
    }

    if (dir_size == 0 || dir_size > NK_ISO_MAX_DIR_BYTES) return -1;
    uint64_t offset = (uint64_t)dir_lba * SECTOR_SIZE;
    if (offset > reader->file_size || (uint64_t)dir_size > reader->file_size - offset) {
        return -1;
    }

    uint8_t *buf = (uint8_t *)malloc(dir_size);
    if (!buf) return -1;

    if (nk_iso_fseek64(reader->f, (int64_t)offset, SEEK_SET) != 0 ||
        fread(buf, 1, dir_size, reader->f) != dir_size) {
        free(buf);
        return -1;
    }

    uint32_t cur_index = 0;
    bool found = false;
    size_t pos = 0;

    while (pos < dir_size) {
        uint8_t rec_len = buf[pos];
        if (rec_len == 0) {
            pos = ((pos / SECTOR_SIZE) + 1u) * SECTOR_SIZE;
            continue;
        }
        if (rec_len < 34u || rec_len > dir_size - pos || (pos % SECTOR_SIZE) + rec_len > SECTOR_SIZE) {
            break;
        }
        uint8_t name_len = buf[pos + 32];
        if (name_len == 0 || 33u + name_len > rec_len) {
            break;
        }

        /* Skip '.' (0) and '..' (1) */
        const uint8_t *name_bytes = &buf[pos + 33];
        if (name_len == 1 && (name_bytes[0] == 0 || name_bytes[0] == 1)) {
            pos += rec_len;
            continue;
        }

        uint32_t ext_lba_le = read_le32(&buf[pos + 2]);
        uint32_t ext_lba_be = read_be32(&buf[pos + 6]);
        uint32_t ext_sz_le = read_le32(&buf[pos + 10]);
        uint32_t ext_sz_be = read_be32(&buf[pos + 14]);

        if (ext_lba_le != ext_lba_be || ext_sz_le != ext_sz_be) {
            pos += rec_len;
            continue;
        }

        uint64_t ext_off = (uint64_t)ext_lba_le * SECTOR_SIZE;
        if (ext_off > reader->file_size || (uint64_t)ext_sz_le > reader->file_size - ext_off) {
            pos += rec_len;
            continue;
        }

        if (cur_index == index) {
            memset(out_entry, 0, sizeof(*out_entry));
            size_t comp_len = name_len;
            for (size_t c = 0; c < name_len; c++) {
                if (name_bytes[c] == ';') { comp_len = c; break; }
            }
            if (comp_len > 0 && name_bytes[comp_len - 1] == '.') {
                comp_len--;
            }
            if (comp_len >= sizeof(out_entry->name)) {
                comp_len = sizeof(out_entry->name) - 1;
            }
            memcpy(out_entry->name, name_bytes, comp_len);
            out_entry->name[comp_len] = '\0';
            out_entry->lba = ext_lba_le;
            out_entry->is_directory = (buf[pos + 25] & 0x02u) != 0;
            out_entry->size = out_entry->is_directory ? 0 : ext_sz_le;
            found = true;
            break;
        }

        cur_index++;
        pos += rec_len;
    }

    free(buf);
    return found ? 1 : 0;
}

#ifndef NK_ISO_NO_PLAYER_EXTRAS
/* SFO parameter formats. 0x0004 is UTF-8 that is not NUL-terminated and
   0x0204 is NUL-terminated UTF-8; 0x0404 is a little-endian uint32. Only the
   two string forms carry text. */
#define SFO_FMT_UTF8_SPECIAL 0x0004u
#define SFO_FMT_UTF8         0x0204u

static bool sfo_key_is_identity(const char *key) {
    return strcmp(key, "DISC_ID") == 0
        || strcmp(key, "TITLE_ID") == 0
        || strcmp(key, "TITLE") == 0
        || strcmp(key, "DISC_VERSION") == 0;
}

/* Parse SFO buffer and extract DISC_ID, TITLE, DISC_VERSION with rigorous bounds checking */
static bool parse_sfo_buffer(const uint8_t *sfo, size_t sfo_size, NkIsoMetadata *meta) {
    if (!sfo || sfo_size < 20 || !meta) return false;

    if (memcmp(sfo, SFO_MAGIC, 4) != 0) {
        return false;
    }

    uint32_t key_table_off = read_le32(sfo + 8);
    uint32_t data_table_off = read_le32(sfo + 12);
    uint32_t entry_count = read_le32(sfo + 16);

    /* Validate header offsets and table bounds */
    if (entry_count > 256 || key_table_off >= sfo_size || data_table_off >= sfo_size) {
        return false;
    }
    if (key_table_off > data_table_off) {
        return false;
    }
    if (20 + (size_t)entry_count * 16 > key_table_off) {
        return false;
    }

    for (uint32_t i = 0; i < entry_count; i++) {
        size_t entry_off = 20 + (size_t)i * 16;
        if (entry_off + 16 > key_table_off) break;

        uint16_t key_off = read_le16(sfo + entry_off);
        uint16_t data_fmt = read_le16(sfo + entry_off + 2);
        uint32_t data_len = read_le32(sfo + entry_off + 4);
        uint32_t data_off = read_le32(sfo + entry_off + 12);

        /* Validate key offset and ensure null termination */
        size_t abs_key_off = (size_t)key_table_off + key_off;
        if (abs_key_off >= data_table_off) continue;
        const char *key = (const char *)&sfo[abs_key_off];
        size_t max_key_len = data_table_off - abs_key_off;
        size_t actual_key_len = 0;
        while (actual_key_len < max_key_len && key[actual_key_len] != '\0') {
            actual_key_len++;
        }
        if (actual_key_len == max_key_len) {
            /* Key string was not null-terminated before data table */
            continue;
        }

        /* Validate data offset and length bounds */
        size_t abs_data_off = (size_t)data_table_off + data_off;
        if (abs_data_off > sfo_size || (size_t)data_len > sfo_size - abs_data_off) {
            continue;
        }

        /* The parameter format field was ignored, so an entry declaring an
           integer or any unrecognised format still had its bytes decoded as
           text. tools/nk_core/iso_inspect.py does not do that -- it yields the
           decoded integer or an empty string -- so a disc whose DISC_ID
           declared a non-string format could be matched to a catalog title by
           the native reader and not by the canonical one, which is exactly the
           divergence the two parsers exist to prevent. Identity fields are
           decoded only from a UTF-8 string format. */
        if (sfo_key_is_identity(key)
            && data_fmt != SFO_FMT_UTF8
            && data_fmt != SFO_FMT_UTF8_SPECIAL) {
            snprintf(meta->error_message, sizeof(meta->error_message),
                "SFO key '%.32s' declares parameter format 0x%04x, which is not a UTF-8 "
                "string; identity is not decoded from a non-string format",
                key, (unsigned)data_fmt);
            return false;
        }

        char val_buf[256];
        int copy_len = (int)data_len;
        if (copy_len >= (int)sizeof(val_buf)) {
            copy_len = (int)sizeof(val_buf) - 1;
        }
        snprintf(val_buf, sizeof(val_buf), "%.*s", copy_len, (const char *)&sfo[abs_data_off]);

        if (strcmp(key, "DISC_ID") == 0 || strcmp(key, "TITLE_ID") == 0) {
            if (meta->disc_id[0] != '\0') {
                if (strcmp(meta->disc_id, val_buf) != 0) {
                    snprintf(meta->error_message, sizeof(meta->error_message),
                        "Conflicting duplicate SFO key '%.32s' rejected as ambiguous: '%.32s' vs '%.32s'",
                        key, meta->disc_id, val_buf);
                    return false;
                }
                /* Identical duplicate: accept per policy */
                continue;
            }
            snprintf(meta->disc_id, sizeof(meta->disc_id), "%.31s", val_buf);
        } else if (strcmp(key, "TITLE") == 0) {
            if (meta->title_name[0] != '\0') {
                if (strcmp(meta->title_name, val_buf) != 0) {
                    snprintf(meta->error_message, sizeof(meta->error_message),
                        "Conflicting duplicate SFO key '%.32s' rejected as ambiguous: '%.64s' vs '%.64s'",
                        key, meta->title_name, val_buf);
                    return false;
                }
                /* Identical duplicate: accept per policy */
                continue;
            }
            snprintf(meta->title_name, sizeof(meta->title_name), "%.127s", val_buf);
        } else if (strcmp(key, "DISC_VERSION") == 0) {
            if (meta->disc_version[0] != '\0') {
                if (strcmp(meta->disc_version, val_buf) != 0) {
                    snprintf(meta->error_message, sizeof(meta->error_message),
                        "Conflicting duplicate SFO key '%.32s' rejected as ambiguous: '%.16s' vs '%.16s'",
                        key, meta->disc_version, val_buf);
                    return false;
                }
                /* Identical duplicate: accept per policy */
                continue;
            }
            snprintf(meta->disc_version, sizeof(meta->disc_version), "%.15s", val_buf);
        }
    }

    return true;
}

/* Helper to scan buffer for standard PSP disc ID strings */
static bool scan_disc_id_in_buffer(const uint8_t *buf, size_t buf_size, char *out_disc_id, size_t max_len) {
    static const char *prefixes[] = {
        "UCUS", "ULUS", "UCES", "ULES", "UCJS", "ULJS", "UCAS", "ULAS", "TEST", NULL
    };

    for (size_t i = 0; i + 9 <= buf_size; i++) {
        for (int p = 0; prefixes[p]; p++) {
            if (memcmp(buf + i, prefixes[p], 4) == 0) {
                size_t pos = i + 4;
                if (pos < buf_size && (buf[pos] == '-' || buf[pos] == '_')) {
                    pos++;
                }
                bool digits = true;
                for (int d = 0; d < 5; d++) {
                    if (pos + d >= buf_size || !isdigit(buf[pos + d])) {
                        digits = false;
                        break;
                    }
                }
                if (digits) {
                    snprintf(out_disc_id, max_len, "%s%.5s", prefixes[p], (const char *)&buf[pos]);
                    return true;
                }
            }
        }
    }
    return false;
}

NkResult nk_iso_inspect(const char *iso_path, NkIsoMetadata *out_meta) {
    if (!iso_path || !out_meta) return NK_ERROR_GENERIC;
    memset(out_meta, 0, sizeof(*out_meta));
    /* disc_version is deliberately left empty here. parse_sfo_buffer treats a
       non-empty value as evidence that a DISC_VERSION key was already seen, so
       pre-seeding a default made the first genuine entry look like a
       conflicting duplicate and rejected every disc whose version was not
       "1.00". The default is applied after parsing, only if the key was
       actually absent. */

    if (!nk_platform_file_exists(iso_path)) {
        snprintf(out_meta->error_message, sizeof(out_meta->error_message), "ISO file does not exist: %s", iso_path);
        return NK_ERROR_FILE_NOT_FOUND;
    }

    FILE *f = nk_iso_fopen(iso_path, "rb");
    if (!f) {
        snprintf(out_meta->error_message, sizeof(out_meta->error_message), "Could not open ISO file: %s", iso_path);
        return NK_ERROR_IO;
    }

    nk_fseek64(f, 0, SEEK_END);
    out_meta->file_size_bytes = nk_ftell64(f);
    nk_fseek64(f, 0, SEEK_SET);

    uint64_t file_size = (uint64_t)out_meta->file_size_bytes;

    if (file_size < (uint64_t)(PVD_SECTOR + 1) * SECTOR_SIZE) {
        fclose(f);
        snprintf(out_meta->error_message, sizeof(out_meta->error_message), "File is too small to be a valid ISO9660 image");
        return NK_ERROR_INVALID_ISO;
    }

    /* Read PVD at sector 16 */
    uint8_t pvd[SECTOR_SIZE];
    nk_fseek64(f, (int64_t)PVD_SECTOR * SECTOR_SIZE, SEEK_SET);
    if (fread(pvd, 1, SECTOR_SIZE, f) != SECTOR_SIZE) {
        fclose(f);
        snprintf(out_meta->error_message, sizeof(out_meta->error_message), "Failed reading Primary Volume Descriptor");
        return NK_ERROR_INVALID_ISO;
    }

    if (pvd[0] != 0x01 || memcmp(&pvd[1], "CD001", 5) != 0) {
        fclose(f);
        snprintf(out_meta->error_message, sizeof(out_meta->error_message), "Not a valid ISO9660 image (missing CD001)");
        return NK_ERROR_INVALID_ISO;
    }

    /* Extract Volume ID (bytes 40..71) */
    char vol_id[33];
    memset(vol_id, 0, sizeof(vol_id));
    memcpy(vol_id, &pvd[40], 32);

    /* Trim trailing spaces */
    for (int i = 31; i >= 0; i--) {
        if (vol_id[i] == ' ' || vol_id[i] == '\0') {
            vol_id[i] = '\0';
        } else {
            break;
        }
    }
    snprintf(out_meta->volume_id, sizeof(out_meta->volume_id), "%s", vol_id);

    /* Search for PARAM.SFO by reading root directory and PSP_GAME */
    uint32_t root_lba = read_le32(&pvd[158]);
    uint32_t root_size = read_le32(&pvd[166]);
    if (root_size > NK_ISO_MAX_DIR_BYTES || root_size == 0) root_size = 4 * SECTOR_SIZE;

    /* Check root directory 64-bit bounds */
    uint64_t root_offset = (uint64_t)root_lba * SECTOR_SIZE;
    if (root_offset > file_size || (uint64_t)root_size > file_size - root_offset) {
        fclose(f);
        snprintf(out_meta->error_message, sizeof(out_meta->error_message), "Root directory extends beyond ISO file bounds");
        return NK_ERROR_INVALID_ISO;
    }

    uint8_t *dir_buf = (uint8_t *)malloc(root_size);
    uint32_t psp_game_lba = 0;
    uint32_t psp_game_size = 0;

    if (dir_buf) {
        nk_fseek64(f, (int64_t)root_offset, SEEK_SET);
        size_t read_bytes = fread(dir_buf, 1, root_size, f);
        size_t off = 0;
        while (off < read_bytes) {
            uint8_t rec_len = dir_buf[off];
            if (rec_len == 0) {
                off = ((off / SECTOR_SIZE) + 1) * SECTOR_SIZE;
                continue;
            }
            /* ECMA-119 6.8.1.1: Directory record must not cross sector boundary */
            if ((off % SECTOR_SIZE) + rec_len > SECTOR_SIZE) {
                snprintf(out_meta->error_message, sizeof(out_meta->error_message),
                    "Malformed ISO: directory record length %u at offset %zu crosses 2048-byte sector boundary (ECMA-119 6.8.1.1)",
                    (unsigned int)rec_len, off);
                free(dir_buf);
                fclose(f);
                return NK_ERROR_INVALID_ISO;
            }
            if (off + rec_len > read_bytes) {
                snprintf(out_meta->error_message, sizeof(out_meta->error_message),
                    "Malformed ISO: directory record truncated before sector end");
                free(dir_buf);
                fclose(f);
                return NK_ERROR_INVALID_ISO;
            }

            uint32_t extent_lba_le = read_le32(&dir_buf[off + 2]);
            uint32_t extent_lba_be = read_be32(&dir_buf[off + 6]);
            uint32_t extent_size_le = read_le32(&dir_buf[off + 10]);
            uint32_t extent_size_be = read_be32(&dir_buf[off + 14]);

            /* Both-endian verification (ECMA-119 7.3.3) */
            if (extent_lba_le != extent_lba_be || extent_size_le != extent_size_be) {
                off += rec_len;
                continue;
            }

            uint32_t extent_lba = extent_lba_le;
            uint32_t extent_size = extent_size_le;
            uint8_t name_len = dir_buf[off + 32];

            /* Checked arithmetic on candidate extent */
            uint64_t ext_off = (uint64_t)extent_lba * SECTOR_SIZE;
            if (ext_off <= file_size && (uint64_t)extent_size <= file_size - ext_off) {
                if (name_len == 8 && memcmp(&dir_buf[off + 33], "PSP_GAME", 8) == 0) {
                    psp_game_lba = extent_lba;
                    psp_game_size = extent_size;
                    break;
                }
            }
            off += rec_len;
        }
        free(dir_buf);
    }

    uint32_t sfo_lba = 0;
    uint32_t sfo_size = 0;

    /* True only when disc_id came from a parsed, validated PARAM.SFO structure.
       A disc id recovered by scanning raw image bytes is a guess: the byte
       sequence "TEST00001" occurring anywhere in 64 MiB of image data is not
       evidence that this disc is that title. Such an id may be reported, but it
       must never satisfy the catalog and mark the disc supported/verified. */
    bool identity_structured = false;

    if (psp_game_lba > 0) {
        if (psp_game_size > NK_ISO_MAX_DIR_BYTES || psp_game_size == 0) psp_game_size = 4 * SECTOR_SIZE;
        uint64_t psp_game_offset = (uint64_t)psp_game_lba * SECTOR_SIZE;

        if (psp_game_offset <= file_size && (uint64_t)psp_game_size <= file_size - psp_game_offset) {
            dir_buf = (uint8_t *)malloc(psp_game_size);
            if (dir_buf) {
                nk_fseek64(f, (int64_t)psp_game_offset, SEEK_SET);
                size_t read_bytes = fread(dir_buf, 1, psp_game_size, f);
                size_t off = 0;
                while (off < read_bytes) {
                    uint8_t rec_len = dir_buf[off];
                    if (rec_len == 0) {
                        off = ((off / SECTOR_SIZE) + 1) * SECTOR_SIZE;
                        continue;
                    }
                    /* ECMA-119 6.8.1.1: Directory record must not cross sector boundary */
                    if ((off % SECTOR_SIZE) + rec_len > SECTOR_SIZE) {
                        snprintf(out_meta->error_message, sizeof(out_meta->error_message),
                            "Malformed ISO: PSP_GAME directory record length %u at offset %zu crosses 2048-byte sector boundary (ECMA-119 6.8.1.1)",
                            (unsigned int)rec_len, off);
                        free(dir_buf);
                        fclose(f);
                        return NK_ERROR_INVALID_ISO;
                    }
                    if (off + rec_len > read_bytes) {
                        snprintf(out_meta->error_message, sizeof(out_meta->error_message),
                            "Malformed ISO: PSP_GAME directory record truncated before sector end");
                        free(dir_buf);
                        fclose(f);
                        return NK_ERROR_INVALID_ISO;
                    }

                    uint32_t extent_lba_le = read_le32(&dir_buf[off + 2]);
                    uint32_t extent_lba_be = read_be32(&dir_buf[off + 6]);
                    uint32_t extent_size_le = read_le32(&dir_buf[off + 10]);
                    uint32_t extent_size_be = read_be32(&dir_buf[off + 14]);

                    /* Both-endian verification (ECMA-119 7.3.3) */
                    if (extent_lba_le != extent_lba_be || extent_size_le != extent_size_be) {
                        off += rec_len;
                        continue;
                    }

                    uint32_t extent_lba = extent_lba_le;
                    uint32_t extent_size = extent_size_le;
                    uint8_t name_len = dir_buf[off + 32];

                    uint64_t ext_off = (uint64_t)extent_lba * SECTOR_SIZE;
                    if (ext_off <= file_size && (uint64_t)extent_size <= file_size - ext_off) {
                        /* ECMA-119 7.5.1: a file identifier is NAME;VERSION, so
                           "PARAM.SFO" may legitimately appear as "PARAM.SFO;1".
                           Accept only those two forms -- a bare prefix test also
                           matches PARAM.SFOO and any longer name sharing it. */
                        if (name_len >= 9 && memcmp(&dir_buf[off + 33], "PARAM.SFO", 9) == 0
                            && (name_len == 9 || dir_buf[off + 33 + 9] == ';')) {
                            sfo_lba = extent_lba;
                            sfo_size = extent_size;
                            break;
                        }
                    }
                    off += rec_len;
                }
                free(dir_buf);
            }
        }
    }

    /* Read SFO if located via directory traversal with bounds validation */
    if (sfo_lba > 0 && sfo_size > 0 && sfo_size <= 64 * 1024) {
        uint64_t sfo_offset = (uint64_t)sfo_lba * SECTOR_SIZE;
        if (sfo_offset <= file_size && (uint64_t)sfo_size <= file_size - sfo_offset) {
            uint8_t *sfo_buf = (uint8_t *)malloc(sfo_size);
            if (sfo_buf) {
                nk_fseek64(f, (int64_t)sfo_offset, SEEK_SET);
                if (fread(sfo_buf, 1, sfo_size, f) == sfo_size) {
                    if (parse_sfo_buffer(sfo_buf, sfo_size, out_meta)) {
                        out_meta->param_sfo_parsed = true;
                        if (out_meta->disc_id[0] != 0) identity_structured = true;
                    } else {
                        if (out_meta->error_message[0] != '\0') {
                            free(sfo_buf);
                            fclose(f);
                            return NK_ERROR_INVALID_ISO;
                        }
                    }
                }
                free(sfo_buf);
            }
        }
    }

    /* Fallback scan if SFO was not found in directory traversal (e.g. synthetic test ISO) */
    if (out_meta->disc_id[0] == '\0') {
        size_t scan_size = (size_t)(file_size < (64 * 1024 * 1024) ? file_size : (64 * 1024 * 1024));
        uint8_t *scan_buf = (uint8_t *)malloc(scan_size);
        if (scan_buf) {
            nk_fseek64(f, 0, SEEK_SET);
            size_t bytes_read = fread(scan_buf, 1, scan_size, f);
            /* Search for SFO magic */
            for (size_t i = 0; i + 20 <= bytes_read; i++) {
                if (memcmp(scan_buf + i, SFO_MAGIC, 4) == 0) {
                    if (parse_sfo_buffer(scan_buf + i, bytes_read - i, out_meta)) {
                        /* Deliberately does NOT set identity_structured. An SFO
                           found by scanning raw bytes has no filesystem
                           provenance: any image can embed a valid SFO blob
                           inside unrelated file data, and trusting it would let
                           a crafted disc be matched to a catalog title and
                           launch title-specific code against the wrong content.
                           The parsed values stay informational; only an SFO
                           reached through a validated directory extent may
                           authorize catalog matching. */
                    } else {
                        if (out_meta->error_message[0] != '\0') {
                            free(scan_buf);
                            fclose(f);
                            return NK_ERROR_INVALID_ISO;
                        }
                    }
                    if (out_meta->disc_id[0] != '\0') break;
                }
            }
            /* If still not found, scan for known disc ID pattern */
            if (out_meta->disc_id[0] == '\0') {
                scan_disc_id_in_buffer(scan_buf, bytes_read, out_meta->disc_id, sizeof(out_meta->disc_id));
            }
            free(scan_buf);
        }
    }

    fclose(f);

    if (out_meta->disc_id[0] == '\0') {
        if (out_meta->volume_id[0] != '\0') {
            snprintf(out_meta->disc_id, sizeof(out_meta->disc_id), "%.*s", (int)(sizeof(out_meta->disc_id) - 1), out_meta->volume_id);
            snprintf(out_meta->title_name, sizeof(out_meta->title_name), "%s", out_meta->volume_id);
        } else {
            snprintf(out_meta->disc_id, sizeof(out_meta->disc_id), "UNKNOWN");
            snprintf(out_meta->title_name, sizeof(out_meta->title_name), "Unknown PSP Disc");
        }
    }

    /* The SFO carries no DISC_VERSION: record the conventional default now
       that parsing can no longer mistake it for a duplicate key. */
    if (out_meta->disc_version[0] == '\0') {
        snprintf(out_meta->disc_version, sizeof(out_meta->disc_version), "1.00");
    }

    /* Look up in native title catalog */
    const NkTitleEntry *entry = identity_structured
        ? nk_title_catalog_find_by_disc_id(out_meta->disc_id)
        : NULL;
    if (entry) {
        out_meta->is_supported = true;
        out_meta->matched_title = entry;
        out_meta->status = NK_STATUS_VERIFIED;
        if (out_meta->title_name[0] == '\0') {
            snprintf(out_meta->title_name, sizeof(out_meta->title_name), "%s", entry->display_name);
        }
    } else {
        out_meta->is_supported = false;
        out_meta->matched_title = NULL;
        out_meta->status = NK_STATUS_IDENTIFIED;
        if (out_meta->title_name[0] == '\0') {
            snprintf(out_meta->title_name, sizeof(out_meta->title_name), "PSP Title (%s)", out_meta->disc_id);
        }
    }

    /* Executable classification is advisory preflight data. A valid disc can
       still be inspected when its SYSDIR files are missing or unsupported. */
    (void)nk_iso_classify_executables(iso_path, &out_meta->executables);

    return NK_OK;
}

static bool nk_iso_checked_add_u64(uint64_t left, uint64_t right,
                                   uint64_t *out_sum) {
    if (!out_sum || right > UINT64_MAX - left) return false;
    *out_sum = left + right;
    return true;
}

static bool nk_iso_extent_span_valid(NkIsoReader *reader, uint32_t lba,
                                     uint32_t size, uint64_t offset,
                                     uint32_t bytes) {
    if (!reader) return false;
    uint64_t file_offset = (uint64_t)lba * SECTOR_SIZE;
    uint64_t file_size = nk_iso_reader_file_size(reader);
    return file_offset <= file_size && (uint64_t)size <= file_size - file_offset &&
           offset <= (uint64_t)size && (uint64_t)bytes <= (uint64_t)size - offset;
}

static bool nk_iso_read_extent_exact(NkIsoReader *reader, uint32_t lba,
                                     uint32_t size, uint64_t offset,
                                     void *dst, uint32_t bytes) {
    if (!dst || !nk_iso_extent_span_valid(reader, lba, size, offset, bytes)) {
        return false;
    }
    return nk_iso_reader_read(reader, lba, offset, dst, bytes) == (int)bytes;
}

static bool nk_iso_elf32_mips_usable(NkIsoReader *reader, uint32_t lba,
                                     uint32_t size) {
    uint8_t header[52];
    if (size < sizeof(header) ||
        !nk_iso_read_extent_exact(reader, lba, size, 0, header, sizeof(header))) {
        return false;
    }
    if (memcmp(header, "\x7f" "ELF", 4) != 0 || header[4] != 1 ||
        header[5] != 1 || header[6] != 1) return false;

    uint16_t type = read_le16(header + 16);
    uint16_t machine = read_le16(header + 18);
    uint32_t version = read_le32(header + 20);
    uint32_t entry = read_le32(header + 24);
    uint32_t phoff = read_le32(header + 28);
    uint32_t shoff = read_le32(header + 32);
    uint16_t ehsize = read_le16(header + 40);
    uint16_t phentsize = read_le16(header + 42);
    uint16_t phnum = read_le16(header + 44);
    uint16_t shentsize = read_le16(header + 46);
    uint16_t shnum = read_le16(header + 48);
    uint64_t phbytes = (uint64_t)phentsize * phnum;
    uint64_t phend = 0;

    /* The analyzer takes relocatable (1), executable (2), shared (3) and the
       PSP PRX format (0xFFA0); many retail BOOT.BIN images are PRX-format. */
    if ((type != 1 && type != 2 && type != 3 && type != 0xFFA0u) ||
        machine != 8 || version != 1 ||
        ehsize != sizeof(header) || phentsize != 32 || phnum == 0 ||
        phnum > NK_ISO_MAX_ELF_PROGRAM_HEADERS || phoff < ehsize ||
        !nk_iso_checked_add_u64(phoff, phbytes, &phend) || phend > size) {
        return false;
    }
    if (shnum != 0) {
        uint64_t shbytes = (uint64_t)shentsize * shnum;
        uint64_t shend = 0;
        if (shentsize != 40 || shoff < ehsize ||
            !nk_iso_checked_add_u64(shoff, shbytes, &shend) || shend > size) {
            return false;
        }
    } else if (shoff != 0) {
        return false;
    }

    uint8_t program_headers[NK_ISO_MAX_ELF_PROGRAM_HEADERS * 32u];
    if (!nk_iso_read_extent_exact(reader, lba, size, phoff, program_headers,
                                  (uint32_t)phbytes)) return false;

    bool have_load = false;
    bool entry_executable = false;
    for (uint16_t i = 0; i < phnum; i++) {
        const uint8_t *ph = program_headers + (size_t)i * phentsize;
        uint32_t p_type = read_le32(ph + 0);
        uint32_t p_offset = read_le32(ph + 4);
        uint32_t p_vaddr = read_le32(ph + 8);
        uint32_t p_filesz = read_le32(ph + 16);
        uint32_t p_memsz = read_le32(ph + 20);
        uint32_t p_flags = read_le32(ph + 24);
        uint32_t p_align = read_le32(ph + 28);
        uint64_t file_end = 0;
        uint64_t memory_end = 0;

        if (!nk_iso_checked_add_u64(p_offset, p_filesz, &file_end) ||
            file_end > size) return false;
        if (p_type != 1) continue;
        if (p_memsz < p_filesz ||
            !nk_iso_checked_add_u64(p_vaddr, p_memsz, &memory_end) ||
            memory_end > (uint64_t)UINT32_MAX + 1u) return false;
        if (p_align > 1 &&
            ((p_align & (p_align - 1u)) != 0 ||
             (p_offset % p_align) != (p_vaddr % p_align))) return false;
        have_load = true;
        if ((p_flags & 1u) != 0 && entry >= p_vaddr &&
            (uint64_t)entry < memory_end) entry_executable = true;
    }
    return have_load && entry_executable;
}

static bool nk_iso_extent_is_zero_filled(NkIsoReader *reader, uint32_t lba,
                                         uint32_t size) {
    uint8_t buffer[8192];
    uint64_t offset = 0;
    while (offset < size) {
        uint64_t remaining = (uint64_t)size - offset;
        uint32_t count = (uint32_t)(remaining < sizeof(buffer)
            ? remaining : sizeof(buffer));
        if (!nk_iso_read_extent_exact(reader, lba, size, offset, buffer, count)) {
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            if (buffer[i] != 0) return false;
        }
        offset += count;
    }
    return true;
}

static void nk_iso_classify_executable(NkIsoReader *reader, const char *path,
                                       NkIsoExecutableCandidate *out) {
    uint32_t lba = 0, size = 0;
    bool is_directory = false;
    uint8_t header[0x80] = { 0 };
    uint32_t header_size;

    memset(out, 0, sizeof(*out));
    out->kind = NK_ISO_EXEC_UNKNOWN;
    if (nk_iso_reader_lookup(reader, path, &lba, &size, &is_directory) != 0 ||
        is_directory || !nk_iso_extent_span_valid(reader, lba, size, 0, size)) {
        return;
    }
    out->present = true;
    out->size_bytes = size;
    if (size == 0) {
        out->kind = NK_ISO_EXEC_EMPTY_OR_ZERO;
        return;
    }
    if (size > NK_ISO_MAX_EXECUTABLE_BYTES) return;

    header_size = size < sizeof(header) ? size : (uint32_t)sizeof(header);
    if (!nk_iso_read_extent_exact(reader, lba, size, 0, header, header_size)) return;
    if (header_size >= 4 && memcmp(header, "\x7f" "ELF", 4) == 0) {
        if (nk_iso_elf32_mips_usable(reader, lba, size)) {
            out->kind = NK_ISO_EXEC_MIPS_ELF32;
        }
        return;
    }
    if (header_size >= 4 && memcmp(header, "~SCE", 4) == 0) {
        out->kind = NK_ISO_EXEC_SCE_WRAPPER;
        return;
    }
    if (header_size >= 4 && memcmp(header, "\0PBP", 4) == 0) {
        out->kind = NK_ISO_EXEC_PBP;
        return;
    }
    if (header_size >= 4 && memcmp(header, "~PSP", 4) == 0) {
        if (header_size < 0x64 || header[0x27] == 0 || header[0x27] > 4) return;
        uint64_t segment_total = 0;
        for (unsigned i = 0; i < header[0x27]; i++) {
            uint32_t segment_size = read_le32(header + 0x54 + i * 4u);
            if (segment_size == 0 || segment_size > NK_ISO_MAX_EXECUTABLE_BYTES ||
                segment_total > NK_ISO_MAX_EXECUTABLE_BYTES - segment_size) return;
            segment_total += segment_size;
        }
        out->kind = NK_ISO_EXEC_PSP_ENCRYPTED;
        return;
    }
    if (nk_iso_extent_is_zero_filled(reader, lba, size)) {
        out->kind = NK_ISO_EXEC_EMPTY_OR_ZERO;
    }
}

NkResult nk_iso_classify_executables(const char *iso_path,
                                     NkIsoExecutableReport *out_report) {
    if (!iso_path || !out_report) return NK_ERROR_GENERIC;
    memset(out_report, 0, sizeof(*out_report));
    NkIsoReader *reader = nk_iso_reader_open(iso_path);
    if (!reader) return NK_ERROR_INVALID_ISO;

    nk_iso_classify_executable(reader, "PSP_GAME/SYSDIR/EBOOT.BIN",
                               &out_report->eboot);
    nk_iso_classify_executable(reader, "PSP_GAME/SYSDIR/BOOT.BIN",
                               &out_report->boot);
    nk_iso_reader_close(reader);

    if (out_report->eboot.kind == NK_ISO_EXEC_MIPS_ELF32) {
        out_report->selected = NK_ISO_EXEC_SELECTION_EBOOT;
        snprintf(out_report->selected_path, sizeof(out_report->selected_path),
                 "EBOOT.BIN");
    } else if (out_report->eboot.kind == NK_ISO_EXEC_PSP_ENCRYPTED &&
               out_report->boot.kind == NK_ISO_EXEC_MIPS_ELF32) {
        out_report->selected = NK_ISO_EXEC_SELECTION_BOOT;
        out_report->boot_fallback = true;
        snprintf(out_report->selected_path, sizeof(out_report->selected_path),
                 "BOOT.BIN");
    }
    return NK_OK;
}

NkResult nk_iso_extract_file(const char *iso_path, const char *disc_rel_path, const char *host_dest_path) {
    if (!iso_path || !disc_rel_path || !host_dest_path) return NK_ERROR_GENERIC;

    FILE *f_iso = nk_iso_fopen(iso_path, "rb");
    if (!f_iso) return NK_ERROR_FILE_NOT_FOUND;

    nk_fseek64(f_iso, 0, SEEK_END);
    int64_t raw_sz = nk_ftell64(f_iso);
    nk_fseek64(f_iso, 0, SEEK_SET);

    if (raw_sz <= 0) {
        fclose(f_iso);
        return NK_ERROR_INVALID_ISO;
    }
    uint64_t file_size = (uint64_t)raw_sz;

    uint8_t pvd[SECTOR_SIZE];
    nk_fseek64(f_iso, (int64_t)PVD_SECTOR * SECTOR_SIZE, SEEK_SET);
    if (fread(pvd, 1, SECTOR_SIZE, f_iso) != SECTOR_SIZE || pvd[0] != 0x01) {
        fclose(f_iso);
        return NK_ERROR_INVALID_ISO;
    }

    /* Parse root dir */
    uint32_t cur_lba = read_le32(&pvd[158]);
    uint32_t cur_size = read_le32(&pvd[166]);
    if (cur_size > NK_ISO_MAX_DIR_BYTES || cur_size == 0) cur_size = 4 * SECTOR_SIZE;

    uint64_t cur_offset = (uint64_t)cur_lba * SECTOR_SIZE;
    if (cur_offset > file_size || (uint64_t)cur_size > file_size - cur_offset) {
        fclose(f_iso);
        return NK_ERROR_INVALID_ISO;
    }

    /* Parse path segments */
    char path_copy[512];
    snprintf(path_copy, sizeof(path_copy), "%s", disc_rel_path);

    char *saveptr = NULL;
    char *token = strtok_r(path_copy, "/\\", &saveptr);
    uint32_t target_lba = 0;
    uint32_t target_size = 0;

    int depth = 0;
    while (token != NULL) {
        if (++depth > NK_ISO_MAX_DEPTH) {
            /* Maximum directory depth exceeded: reject potential loop/attack */
            fclose(f_iso);
            return NK_ERROR_INVALID_ISO;
        }

        char *next_token = strtok_r(NULL, "/\\", &saveptr);
        bool is_last = (next_token == NULL);

        if (cur_size > 4 * 1024 * 1024) {
            fclose(f_iso);
            return NK_ERROR_INVALID_ISO;
        }

        cur_offset = (uint64_t)cur_lba * SECTOR_SIZE;
        if (cur_offset > file_size || (uint64_t)cur_size > file_size - cur_offset) {
            fclose(f_iso);
            return NK_ERROR_INVALID_ISO;
        }

        uint8_t *dir_buf = (uint8_t *)malloc(cur_size);
        if (!dir_buf) {
            fclose(f_iso);
            return NK_ERROR_OUT_OF_MEMORY;
        }

        nk_fseek64(f_iso, (int64_t)cur_offset, SEEK_SET);
        size_t read_bytes = fread(dir_buf, 1, cur_size, f_iso);

        bool found = false;
        size_t off = 0;
        while (off < read_bytes) {
            uint8_t rec_len = dir_buf[off];
            if (rec_len == 0) {
                off = ((off / SECTOR_SIZE) + 1) * SECTOR_SIZE;
                continue;
            }
            if (off + rec_len > read_bytes) break;

            /* ECMA-119 6.8.1.1: Directory record must not cross sector boundary */
            if ((off % SECTOR_SIZE) + rec_len > SECTOR_SIZE) {
                off = ((off / SECTOR_SIZE) + 1) * SECTOR_SIZE;
                continue;
            }

            uint32_t ext_lba_le = read_le32(&dir_buf[off + 2]);
            uint32_t ext_lba_be = read_be32(&dir_buf[off + 6]);
            uint32_t ext_size_le = read_le32(&dir_buf[off + 10]);
            uint32_t ext_size_be = read_be32(&dir_buf[off + 14]);

            /* Both-endian verification (ECMA-119 7.3.3) */
            if (ext_lba_le != ext_lba_be || ext_size_le != ext_size_be) {
                off += rec_len;
                continue;
            }

            uint32_t ext_lba = ext_lba_le;
            uint32_t ext_size = ext_size_le;
            uint8_t name_len = dir_buf[off + 32];
            const char *name = (const char *)&dir_buf[off + 33];

            /* Strip ;1 version from ISO9660 filename if present */
            size_t comp_len = name_len;
            for (size_t c = 0; c < name_len; c++) {
                if (name[c] == ';') { comp_len = c; break; }
            }

            if (strlen(token) == comp_len && strncasecmp(token, name, comp_len) == 0) {
                cur_lba = ext_lba;
                cur_size = ext_size;
                if (is_last) {
                    target_lba = ext_lba;
                    target_size = ext_size;
                }
                found = true;
                break;
            }
            off += rec_len;
        }
        free(dir_buf);

        if (!found) {
            fclose(f_iso);
            return NK_ERROR_FILE_NOT_FOUND;
        }

        token = next_token;
    }

    if (target_lba == 0 || target_size == 0) {
        fclose(f_iso);
        return NK_ERROR_FILE_NOT_FOUND;
    }

    /* Checked bounds on target file extraction */
    uint64_t target_offset = (uint64_t)target_lba * SECTOR_SIZE;
    if (target_offset > file_size || (uint64_t)target_size > file_size - target_offset) {
        fclose(f_iso);
        return NK_ERROR_INVALID_ISO;
    }

    /* Extract to host_dest_path */
    FILE *f_out = nk_iso_fopen(host_dest_path, "wb");
    if (!f_out) {
        fclose(f_iso);
        return NK_ERROR_IO;
    }

    nk_fseek64(f_iso, (int64_t)target_offset, SEEK_SET);
    uint8_t buf[8192];
    uint32_t remaining = target_size;
    while (remaining > 0) {
        size_t to_read = remaining < sizeof(buf) ? (size_t)remaining : sizeof(buf);
        size_t n = fread(buf, 1, to_read, f_iso);
        if (n == 0) break;
        /* Count what was persisted, not what was read. Ignoring a short write
           let a full destination filesystem consume the whole source and still
           report success with a truncated file. */
        size_t written = fwrite(buf, 1, n, f_out);
        remaining -= (uint32_t)written;
        if (written != n) break;
    }

    /* A close failure can be the first report of a write that never reached
       the disk, so it must fail the extraction rather than be discarded. */
    int close_failed = (fclose(f_out) != 0);
    fclose(f_iso);
    return (remaining == 0 && !close_failed) ? NK_OK : NK_ERROR_IO;
}

/* ------------------------------------------------------------------------- */
/* Native first-time setup payload walk                                      */

#define NK_ISO_STAGE_MAX_VISITED_DIRS 1024u
#define NK_ISO_STAGE_MAX_PATH 4096u
#define NK_ISO_STAGE_MAX_FILES 100000u
#define NK_ISO_STAGE_MAX_BYTES (1ull * 1024ull * 1024ull * 1024ull)

typedef struct {
    uint32_t lba;
    uint32_t size;
    bool is_directory;
    char name[256];
} NkIsoDirectoryEntry;

typedef bool (*NkIsoDirectoryEntryCallback)(const NkIsoDirectoryEntry *entry,
                                             void *userdata);

typedef struct {
    FILE *iso;
    uint64_t file_size;
    const char *host_root;
    NkIsoProgressCallback progress;
    void *progress_userdata;
    uint64_t bytes_complete;
    uint64_t bytes_total;
    size_t files_complete;
    size_t total_files;
    uint32_t visited_lbas[NK_ISO_STAGE_MAX_VISITED_DIRS];
    size_t visited_count;
} NkIsoStageContext;

typedef struct {
    const char *wanted_name;
    NkIsoDirectoryEntry found;
    bool found_any;
    bool conflict;
} NkIsoFindContext;

static bool nk_iso_stage_extent_valid(uint64_t file_size, uint32_t lba,
                                      uint32_t size) {
    uint64_t offset = (uint64_t)lba * SECTOR_SIZE;
    return offset <= file_size && (uint64_t)size <= file_size - offset;
}

static bool nk_iso_stage_component_valid(const uint8_t *name, size_t name_size) {
    if (!name || name_size == 0) return false;
    if ((name_size == 1 && name[0] == '.') ||
        (name_size == 2 && name[0] == '.' && name[1] == '.')) return false;
    for (size_t i = 0; i < name_size; i++) {
        uint8_t value = name[i];
        if (value == 0 || value < 0x20u || value == 0x7fu ||
            value == '/' || value == '\\' || value == '<' || value == '>' ||
            value == ':' || value == '"' || value == '|' || value == '?' ||
            value == '*') return false;
    }
    return true;
}

static bool nk_iso_stage_directory_records(FILE *iso, uint64_t file_size,
                                           uint32_t lba, uint32_t size,
                                           NkIsoDirectoryEntryCallback callback,
                                           void *userdata) {
    if (!iso || !callback || size == 0 || size > NK_ISO_MAX_DIR_BYTES ||
        !nk_iso_stage_extent_valid(file_size, lba, size)) return false;
    uint8_t *buffer = (uint8_t *)malloc(size);
    if (!buffer) return false;
    uint64_t offset = (uint64_t)lba * SECTOR_SIZE;
    bool okay = nk_fseek64(iso, (int64_t)offset, SEEK_SET) == 0 &&
                fread(buffer, 1, size, iso) == size;
    size_t pos = 0;
    while (okay && pos < size) {
        uint8_t record_length = buffer[pos];
        if (record_length == 0) {
            size_t next_sector = ((pos / SECTOR_SIZE) + 1u) * SECTOR_SIZE;
            if (next_sector <= pos) {
                okay = false;
                break;
            }
            pos = next_sector;
            continue;
        }
        if (record_length < 34u || record_length > size - pos ||
            (pos % SECTOR_SIZE) + record_length > SECTOR_SIZE) {
            okay = false;
            break;
        }
        uint8_t name_length = buffer[pos + 32];
        if (name_length == 0 || 33u + name_length > record_length) {
            okay = false;
            break;
        }
        uint32_t extent_lba_le = read_le32(&buffer[pos + 2]);
        uint32_t extent_lba_be = read_be32(&buffer[pos + 6]);
        uint32_t extent_size_le = read_le32(&buffer[pos + 10]);
        uint32_t extent_size_be = read_be32(&buffer[pos + 14]);
        if (extent_lba_le != extent_lba_be || extent_size_le != extent_size_be ||
            !nk_iso_stage_extent_valid(file_size, extent_lba_le, extent_size_le)) {
            okay = false;
            break;
        }

        /* ISO9660 identifiers 0 and 1 are the directory's `.` and `..`
         * records. They are structural links, never payload components. */
        const uint8_t *name_bytes = &buffer[pos + 33];
        if (name_length != 1 || (name_bytes[0] != 0 && name_bytes[0] != 1)) {
            for (size_t i = 0; i < name_length; i++) {
                if (name_bytes[i] == 0) {
                    okay = false;
                    break;
                }
            }
            if (!okay) break;
            size_t component_length = name_length;
            for (size_t i = 0; i < component_length; i++) {
                if (name_bytes[i] == ';') {
                    component_length = i;
                    break;
                }
            }
            if (component_length == 0 || component_length >= 256 ||
                !nk_iso_stage_component_valid(name_bytes, component_length)) {
                okay = false;
                break;
            }
            NkIsoDirectoryEntry entry;
            memset(&entry, 0, sizeof(entry));
            memcpy(entry.name, name_bytes, component_length);
            entry.name[component_length] = '\0';
            entry.lba = extent_lba_le;
            entry.size = extent_size_le;
            entry.is_directory = (buffer[pos + 25] & 0x02u) != 0;
            if (!callback(&entry, userdata)) {
                okay = false;
                break;
            }
        }
        pos += record_length;
    }
    free(buffer);
    return okay;
}

static bool nk_iso_stage_find_callback(const NkIsoDirectoryEntry *entry,
                                       void *userdata) {
    NkIsoFindContext *find = (NkIsoFindContext *)userdata;
    if (!find || !entry || strcasecmp(entry->name, find->wanted_name) != 0) return true;
    if (!find->found_any) {
        find->found = *entry;
        find->found_any = true;
    } else if (find->found.lba != entry->lba || find->found.size != entry->size ||
               find->found.is_directory != entry->is_directory) {
        find->conflict = true;
        return false;
    }
    return true;
}

static NkResult nk_iso_stage_find_child(FILE *iso, uint64_t file_size,
                                        uint32_t directory_lba,
                                        uint32_t directory_size,
                                        const char *name,
                                        NkIsoDirectoryEntry *out_entry) {
    if (!name || !out_entry) return NK_ERROR_GENERIC;
    NkIsoFindContext find;
    memset(&find, 0, sizeof(find));
    find.wanted_name = name;
    if (!nk_iso_stage_directory_records(iso, file_size, directory_lba,
                                        directory_size, nk_iso_stage_find_callback,
                                        &find)) {
        return find.conflict ? NK_ERROR_INVALID_ISO : NK_ERROR_INVALID_ISO;
    }
    if (!find.found_any) return NK_ERROR_FILE_NOT_FOUND;
    *out_entry = find.found;
    return NK_OK;
}

typedef struct {
    NkIsoStageContext *stage;
    bool count_only;
    unsigned depth;
    char relative_prefix[NK_ISO_STAGE_MAX_PATH];
} NkIsoWalkContext;

static NkResult nk_iso_stage_walk_directory(NkIsoWalkContext *walk,
                                            uint32_t directory_lba,
                                            uint32_t directory_size,
                                            unsigned depth);

static bool nk_iso_stage_walk_callback(const NkIsoDirectoryEntry *entry,
                                       void *userdata) {
    NkIsoWalkContext *walk = (NkIsoWalkContext *)userdata;
    if (!walk || !walk->stage || !entry) return false;
    char child_path[NK_ISO_STAGE_MAX_PATH];
    int written = snprintf(child_path, sizeof(child_path), "%s/%s",
                           walk->relative_prefix, entry->name);
    if (written < 0 || (size_t)written >= sizeof(child_path)) return false;

    if (entry->is_directory) {
        NkIsoWalkContext child = *walk;
        snprintf(child.relative_prefix, sizeof(child.relative_prefix), "%s", child_path);
        child.depth = walk->depth + 1u;
        return nk_iso_stage_walk_directory(&child, entry->lba, entry->size,
                                            child.depth) == NK_OK;
    }

    NkIsoStageContext *stage = walk->stage;
    if (walk->count_only) {
        if (stage->total_files >= NK_ISO_STAGE_MAX_FILES ||
            stage->total_files == SIZE_MAX ||
            stage->bytes_total > NK_ISO_STAGE_MAX_BYTES ||
            entry->size > NK_ISO_STAGE_MAX_BYTES - stage->bytes_total) return false;
        stage->total_files++;
        stage->bytes_total += entry->size;
        return true;
    }

    char host_path[NK_ISO_STAGE_MAX_PATH];
    int host_written = snprintf(host_path, sizeof(host_path), "%s%c%s",
                                stage->host_root, nk_platform_path_separator(),
                                child_path);
    if (host_written < 0 || (size_t)host_written >= sizeof(host_path)) return false;
    char parent[NK_ISO_STAGE_MAX_PATH];
    snprintf(parent, sizeof(parent), "%s", host_path);
    char *last_slash = strrchr(parent, '/');
    char *last_backslash = strrchr(parent, '\\');
    if (last_backslash && (!last_slash || last_backslash > last_slash)) last_slash = last_backslash;
    if (!last_slash) return false;
    *last_slash = '\0';
    if (!nk_platform_mkdir_p_private(parent)) return false;

    FILE *out = nk_platform_fopen_private(host_path, "wb");
    if (!out) return false;
    uint64_t source_offset = (uint64_t)entry->lba * SECTOR_SIZE;
    if (nk_fseek64(stage->iso, (int64_t)source_offset, SEEK_SET) != 0) {
        fclose(out);
        remove(host_path);
        return false;
    }
    uint8_t buffer[64 * 1024];
    uint32_t remaining = entry->size;
    bool okay = true;
    while (remaining > 0) {
        size_t requested = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        size_t read_count = fread(buffer, 1, requested, stage->iso);
        if (read_count == 0) {
            okay = false;
            break;
        }
        size_t write_count = fwrite(buffer, 1, read_count, out);
        if (write_count != read_count) {
            okay = false;
            break;
        }
        remaining -= (uint32_t)read_count;
        stage->bytes_complete += read_count;
        if (stage->progress &&
            !stage->progress(child_path, stage->bytes_complete, stage->bytes_total,
                             stage->files_complete, stage->total_files,
                             stage->progress_userdata)) {
            okay = false;
            break;
        }
    }
    int close_failed = fclose(out) != 0;
    if (!okay || remaining != 0 || close_failed) {
        remove(host_path);
        return false;
    }
    stage->files_complete++;
    if (stage->progress &&
        !stage->progress(child_path, stage->bytes_complete, stage->bytes_total,
                         stage->files_complete, stage->total_files,
                         stage->progress_userdata)) return false;
    return true;
}

static NkResult nk_iso_stage_walk_directory(NkIsoWalkContext *walk,
                                            uint32_t directory_lba,
                                            uint32_t directory_size,
                                            unsigned depth) {
    if (!walk || !walk->stage || depth > NK_ISO_MAX_DEPTH || directory_size == 0) {
        return NK_ERROR_INVALID_ISO;
    }
    NkIsoStageContext *stage = walk->stage;
    for (size_t i = 0; i < stage->visited_count; i++) {
        if (stage->visited_lbas[i] == directory_lba) return NK_ERROR_INVALID_ISO;
    }
    if (stage->visited_count >= NK_ISO_STAGE_MAX_VISITED_DIRS) return NK_ERROR_INVALID_ISO;
    stage->visited_lbas[stage->visited_count++] = directory_lba;
    return nk_iso_stage_directory_records(stage->iso, stage->file_size,
                                          directory_lba, directory_size,
                                          nk_iso_stage_walk_callback, walk)
        ? NK_OK : NK_ERROR_INVALID_ISO;
}

NkResult nk_iso_extract_game(const char *iso_path, const char *host_root,
                             NkIsoProgressCallback progress, void *userdata) {
    if (!iso_path || !host_root || !host_root[0]) return NK_ERROR_GENERIC;
    FILE *iso = nk_iso_fopen(iso_path, "rb");
    if (!iso) return NK_ERROR_FILE_NOT_FOUND;
    if (nk_fseek64(iso, 0, SEEK_END) != 0) {
        fclose(iso);
        return NK_ERROR_IO;
    }
    int64_t raw_size = nk_ftell64(iso);
    if (raw_size <= 0 || nk_fseek64(iso, 0, SEEK_SET) != 0) {
        fclose(iso);
        return NK_ERROR_INVALID_ISO;
    }
    uint64_t file_size = (uint64_t)raw_size;
    uint8_t pvd[SECTOR_SIZE];
    if (file_size < (uint64_t)(PVD_SECTOR + 1) * SECTOR_SIZE ||
        nk_fseek64(iso, (int64_t)PVD_SECTOR * SECTOR_SIZE, SEEK_SET) != 0 ||
        fread(pvd, 1, sizeof(pvd), iso) != sizeof(pvd) ||
        pvd[0] != 0x01 || memcmp(&pvd[1], "CD001", 5) != 0) {
        fclose(iso);
        return NK_ERROR_INVALID_ISO;
    }

    uint32_t root_lba = read_le32(&pvd[158]);
    uint32_t root_size = read_le32(&pvd[166]);
    uint32_t root_lba_be = read_be32(&pvd[162]);
    uint32_t root_size_be = read_be32(&pvd[170]);
    if (root_lba != root_lba_be || root_size != root_size_be ||
        !nk_iso_stage_extent_valid(file_size, root_lba, root_size) ||
        root_size == 0 || root_size > NK_ISO_MAX_DIR_BYTES) {
        fclose(iso);
        return NK_ERROR_INVALID_ISO;
    }

    NkIsoDirectoryEntry psp_game = { 0 };
    NkIsoDirectoryEntry sysdir = { 0 };
    NkIsoDirectoryEntry usrdir = { 0 };
    NkIsoDirectoryEntry xbdata = { 0 };
    NkIsoDirectoryEntry eboot = { 0 };
    NkResult result = nk_iso_stage_find_child(iso, file_size, root_lba,
                                              root_size, "PSP_GAME", &psp_game);
    if (result == NK_OK && (!psp_game.is_directory || psp_game.size == 0)) result = NK_ERROR_INVALID_ISO;
    if (result == NK_OK) result = nk_iso_stage_find_child(iso, file_size, psp_game.lba,
                                                           psp_game.size, "SYSDIR", &sysdir);
    if (result == NK_OK && (!sysdir.is_directory || sysdir.size == 0)) result = NK_ERROR_INVALID_ISO;
    if (result == NK_OK) result = nk_iso_stage_find_child(iso, file_size, sysdir.lba,
                                                           sysdir.size, "EBOOT.BIN", &eboot);
    if (result == NK_OK && (eboot.is_directory || eboot.size == 0)) result = NK_ERROR_INVALID_ISO;
    if (result == NK_OK) result = nk_iso_stage_find_child(iso, file_size, psp_game.lba,
                                                           psp_game.size, "USRDIR", &usrdir);
    if (result == NK_OK && (!usrdir.is_directory || usrdir.size == 0)) result = NK_ERROR_INVALID_ISO;
    if (result == NK_OK) result = nk_iso_stage_find_child(iso, file_size, usrdir.lba,
                                                           usrdir.size, "xbdata", &xbdata);
    if (result == NK_OK && (!xbdata.is_directory || xbdata.size == 0)) result = NK_ERROR_INVALID_ISO;
    if (result != NK_OK) {
        fclose(iso);
        return result;
    }

    if (eboot.size > NK_ISO_STAGE_MAX_BYTES) {
        fclose(iso);
        return NK_ERROR_INVALID_ISO;
    }

    NkIsoStageContext stage;
    memset(&stage, 0, sizeof(stage));
    stage.iso = iso;
    stage.file_size = file_size;
    stage.host_root = host_root;
    stage.progress = progress;
    stage.progress_userdata = userdata;
    stage.total_files = 1;
    stage.bytes_total = eboot.size;

    NkIsoWalkContext count_walk;
    memset(&count_walk, 0, sizeof(count_walk));
    count_walk.stage = &stage;
    count_walk.count_only = true;
    count_walk.depth = 1;
    snprintf(count_walk.relative_prefix, sizeof(count_walk.relative_prefix), "xbdata");
    result = nk_iso_stage_walk_directory(&count_walk, xbdata.lba, xbdata.size, 1);
    if (result != NK_OK || stage.total_files == 1) {
        fclose(iso);
        return result == NK_OK ? NK_ERROR_FILE_NOT_FOUND : result;
    }

    if (!nk_platform_mkdir_p_private(host_root)) {
        fclose(iso);
        return NK_ERROR_IO;
    }
    char eboot_path[NK_ISO_STAGE_MAX_PATH];
    int eboot_written = snprintf(eboot_path, sizeof(eboot_path), "%s%cEBOOT.BIN",
                                 host_root, nk_platform_path_separator());
    if (eboot_written < 0 || (size_t)eboot_written >= sizeof(eboot_path)) {
        fclose(iso);
        return NK_ERROR_IO;
    }
    FILE *eboot_out = nk_platform_fopen_private(eboot_path, "wb");
    if (!eboot_out || nk_fseek64(iso, (int64_t)eboot.lba * SECTOR_SIZE, SEEK_SET) != 0) {
        if (eboot_out) fclose(eboot_out);
        fclose(iso);
        return NK_ERROR_IO;
    }
    uint8_t eboot_buffer[64 * 1024];
    uint32_t eboot_remaining = eboot.size;
    while (eboot_remaining > 0) {
        size_t requested = eboot_remaining < sizeof(eboot_buffer)
            ? (size_t)eboot_remaining : sizeof(eboot_buffer);
        size_t read_count = fread(eboot_buffer, 1, requested, iso);
        if (read_count == 0 || fwrite(eboot_buffer, 1, read_count, eboot_out) != read_count) {
            fclose(eboot_out);
            remove(eboot_path);
            fclose(iso);
            return NK_ERROR_IO;
        }
        eboot_remaining -= (uint32_t)read_count;
        stage.bytes_complete += read_count;
        if (stage.progress && !stage.progress("EBOOT.BIN", stage.bytes_complete,
                                               stage.bytes_total, 0, stage.total_files,
                                               stage.progress_userdata)) {
            fclose(eboot_out);
            remove(eboot_path);
            fclose(iso);
            return NK_ERROR_CANCELLED;
        }
    }
    int eboot_close_failed = fclose(eboot_out) != 0;
    if (eboot_close_failed) {
        remove(eboot_path);
        fclose(iso);
        return NK_ERROR_IO;
    }
    stage.files_complete = 1;
    if (stage.progress && !stage.progress("EBOOT.BIN", stage.bytes_complete,
                                           stage.bytes_total, stage.files_complete,
                                           stage.total_files, stage.progress_userdata)) {
        fclose(iso);
        return NK_ERROR_CANCELLED;
    }

    stage.visited_count = 0;
    NkIsoWalkContext extract_walk;
    memset(&extract_walk, 0, sizeof(extract_walk));
    extract_walk.stage = &stage;
    extract_walk.count_only = false;
    extract_walk.depth = 1;
    snprintf(extract_walk.relative_prefix, sizeof(extract_walk.relative_prefix), "xbdata");
    result = nk_iso_stage_walk_directory(&extract_walk, xbdata.lba, xbdata.size, 1);
    fclose(iso);
    return result;
}
#endif /* NK_ISO_NO_PLAYER_EXTRAS */
