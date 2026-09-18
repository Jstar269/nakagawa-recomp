// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <ctype.h>

#include "iso.h"

#define NK_ISO_NO_PLAYER_EXTRAS 1
#include "../core/nk_iso.c"

#if defined(_WIN32) || defined(_WIN64)
static SRWLOCK s_iso_lock = SRWLOCK_INIT;
#define ISO_LOCK()   AcquireSRWLockExclusive(&s_iso_lock)
#define ISO_UNLOCK() ReleaseSRWLockExclusive(&s_iso_lock)
#else
#include <pthread.h>
static pthread_mutex_t s_iso_lock = PTHREAD_MUTEX_INITIALIZER;
#define ISO_LOCK()   pthread_mutex_lock(&s_iso_lock)
#define ISO_UNLOCK() pthread_mutex_unlock(&s_iso_lock)
#endif

static NkIsoReader *s_reader = NULL;
static char s_current_iso_path[1024] = {0};

static IsoDirEntry *s_dir_cache = NULL;
static uint32_t s_dir_cache_count = 0;
static uint32_t s_dir_cache_cap = 0;
static char s_dir_cache_path[512] = {0};
static bool s_dir_cache_valid = false;

static void dir_cache_clear(void) {
    s_dir_cache_count = 0;
    s_dir_cache_valid = false;
    s_dir_cache_path[0] = '\0';
}

static const char *iso_normalize_path(const char *guest_path, char *buf, size_t buf_sz) {
    if (!guest_path || !buf || buf_sz == 0) return NULL;
    const char *p = guest_path;
    const char *colon = strchr(p, ':');
    if (colon) p = colon + 1;
    for (;;) {
        while (*p == '/' || *p == '\\') p++;
        if (p[0] == '.' && (p[1] == '/' || p[1] == '\\')) {
            p += 2;
            continue;
        }
        break;
    }
    size_t len = strlen(p);
    if (len >= buf_sz) return NULL;
    memcpy(buf, p, len + 1);
    while (len > 0 && (buf[len - 1] == '/' || buf[len - 1] == '\\')) {
        buf[--len] = '\0';
    }
    return buf;
}

static const char *get_iso_env(void) {
    const char *iso_env = getenv("PSP_ISO");
#if defined(_WIN32) || defined(_WIN64)
    static char win_env[1024];
    if (!iso_env || !iso_env[0]) {
        DWORD len = GetEnvironmentVariableA("PSP_ISO", win_env, sizeof(win_env));
        if (len > 0 && len < sizeof(win_env)) {
            return win_env;
        }
    }
#endif
    return iso_env;
}

static int iso_init_locked(void) {
    const char *iso_env = get_iso_env();
    if (!iso_env || !iso_env[0]) {
        return -1;
    }
    if (s_reader) {
        if (strcmp(s_current_iso_path, iso_env) == 0) {
            return 0;
        }
        nk_iso_reader_close(s_reader);
        s_reader = NULL;
        s_current_iso_path[0] = '\0';
        dir_cache_clear();
    }
    s_reader = nk_iso_reader_open(iso_env);
    if (!s_reader) {
        return -1;
    }
    strncpy(s_current_iso_path, iso_env, sizeof(s_current_iso_path) - 1);
    s_current_iso_path[sizeof(s_current_iso_path) - 1] = '\0';
    return 0;
}

static int ensure_reader_locked(void) {
    const char *iso_env = get_iso_env();
    if (!iso_env || !iso_env[0]) {
        return -1;
    }
    if (s_reader && strcmp(s_current_iso_path, iso_env) == 0) {
        return 0;
    }
    return iso_init_locked();
}

int iso_init(void) {
    ISO_LOCK();
    int rc = iso_init_locked();
    ISO_UNLOCK();
    return rc;
}

int iso_lookup(const char *guest_path, uint32_t *out_lba, uint32_t *out_size) {
    if (!guest_path || !out_lba || !out_size) return -1;

    char norm_path[512];
    if (!iso_normalize_path(guest_path, norm_path, sizeof(norm_path))) {
        return -1;
    }

    if (norm_path[0] == '\0') {
        return -1;
    }

    size_t orig_len = strlen(guest_path);
    if (orig_len > 0 && (guest_path[orig_len - 1] == '/' || guest_path[orig_len - 1] == '\\')) {
        return -1;
    }

    ISO_LOCK();
    if (ensure_reader_locked() != 0) {
        ISO_UNLOCK();
        return -1;
    }

    uint32_t lba = 0, size = 0;
    bool is_dir = false;
    int rc = nk_iso_reader_lookup(s_reader, norm_path, &lba, &size, &is_dir);
    if (rc != 0 || is_dir) {
        ISO_UNLOCK();
        return -1;
    }

    *out_lba = lba;
    *out_size = size;
    ISO_UNLOCK();
    return 0;
}

int iso_read(uint32_t lba, uint32_t offset, void *dst, uint32_t bytes) {
    if (!dst) return -1;
    if (bytes == 0) return 0;

    ISO_LOCK();
    if (ensure_reader_locked() != 0) {
        ISO_UNLOCK();
        return -1;
    }

    int rc = nk_iso_reader_read(s_reader, lba, (uint64_t)offset, dst, bytes);
    ISO_UNLOCK();

    return rc;
}

uint32_t iso_physical_lba(uint32_t lba_or_token) {
    /* Single extent supported; token is the physical sector LBA directly.
     * Document multi-extent status: multi-extent ISO files would require an
     * extent-map table; this public reader operates on single extents. */
    return lba_or_token;
}

int iso_list(const char *guest_path, uint32_t index, IsoDirEntry *out) {
    if (!guest_path || !out) return -1;

    char norm_path[512];
    if (!iso_normalize_path(guest_path, norm_path, sizeof(norm_path))) {
        return -1;
    }

    ISO_LOCK();
    if (ensure_reader_locked() != 0) {
        ISO_UNLOCK();
        return -1;
    }

    if (!s_dir_cache_valid || strcmp(s_dir_cache_path, norm_path) != 0) {
        dir_cache_clear();

        uint32_t dir_lba = 0;
        uint32_t dir_size = 0;

        if (norm_path[0] == '\0') {
            dir_lba = s_reader->root_lba;
            dir_size = s_reader->root_size;
        } else {
            bool is_dir = false;
            int lookup_rc = nk_iso_reader_lookup(s_reader, norm_path, &dir_lba, &dir_size, &is_dir);
            if (lookup_rc != 0 || !is_dir) {
                ISO_UNLOCK();
                return -1;
            }
        }

        if (dir_size == 0 || dir_size > NK_ISO_MAX_DIR_BYTES) {
            ISO_UNLOCK();
            return -1;
        }

        uint64_t file_size = nk_iso_reader_file_size(s_reader);
        uint64_t dir_off = (uint64_t)dir_lba * 2048ULL;
        if (dir_off > file_size || (uint64_t)dir_size > file_size - dir_off) {
            ISO_UNLOCK();
            return -1;
        }

        uint8_t *buf = (uint8_t *)malloc(dir_size);
        if (!buf) {
            ISO_UNLOCK();
            return -1;
        }

        if (nk_iso_reader_read(s_reader, dir_lba, 0, buf, dir_size) != (int)dir_size) {
            free(buf);
            ISO_UNLOCK();
            return -1;
        }

        uint32_t pos = 0;
        while (pos < dir_size) {
            uint8_t rec_len = buf[pos];
            if (rec_len == 0) {
                pos = ((pos / 2048u) + 1u) * 2048u;
                continue;
            }
            if (rec_len < 34u || rec_len > dir_size - pos || (pos % 2048u) + rec_len > 2048u) {
                break;
            }
            uint8_t name_len = buf[pos + 32];
            if (name_len == 0 || 33u + name_len > rec_len) {
                break;
            }

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

            uint64_t ext_off = (uint64_t)ext_lba_le * 2048ULL;
            if (ext_off > file_size || (uint64_t)ext_sz_le > file_size - ext_off) {
                pos += rec_len;
                continue;
            }

            if (s_dir_cache_count >= s_dir_cache_cap) {
                uint32_t new_cap = s_dir_cache_cap == 0 ? 64 : s_dir_cache_cap * 2;
                IsoDirEntry *new_entries = (IsoDirEntry *)realloc(s_dir_cache, new_cap * sizeof(IsoDirEntry));
                if (!new_entries) {
                    free(buf);
                    ISO_UNLOCK();
                    return -1;
                }
                s_dir_cache = new_entries;
                s_dir_cache_cap = new_cap;
            }

            IsoDirEntry *e = &s_dir_cache[s_dir_cache_count];
            memset(e, 0, sizeof(*e));

            size_t comp_len = name_len;
            for (size_t c = 0; c < name_len; c++) {
                if (name_bytes[c] == ';') { comp_len = c; break; }
            }
            if (comp_len > 0 && name_bytes[comp_len - 1] == '.') {
                comp_len--;
            }
            if (comp_len >= sizeof(e->name)) {
                comp_len = sizeof(e->name) - 1;
            }
            memcpy(e->name, name_bytes, comp_len);
            e->name[comp_len] = '\0';
            e->lba = ext_lba_le;
            e->is_dir = (buf[pos + 25] & 0x02u) != 0 ? 1 : 0;
            e->size = e->is_dir ? 0 : ext_sz_le;
            e->is_symlink = 0;
            s_dir_cache_count++;

            pos += rec_len;
        }

        free(buf);
        strncpy(s_dir_cache_path, norm_path, sizeof(s_dir_cache_path) - 1);
        s_dir_cache_path[sizeof(s_dir_cache_path) - 1] = '\0';
        s_dir_cache_valid = true;
    }

    if (index < s_dir_cache_count) {
        *out = s_dir_cache[index];
        ISO_UNLOCK();
        return 1;
    }

    ISO_UNLOCK();
    return 0;
}
