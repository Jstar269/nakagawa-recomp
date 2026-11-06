/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * pipeline_cache.h - the file that keeps the Vulkan GE renderer's pipeline cache between runs
 * (#2149). ge_gpu.c owns the VkPipelineCache; this header owns the file around its data: the
 * key that says which device, driver and shader set made it, and every check a file must pass
 * before its data reaches the driver.
 *
 * The functions are static so the header adds no translation unit to the GE build lines; it is
 * included by ge_gpu.c and by its CPU selftest (pipeline_cache_selftest.c), which needs no GPU.
 *
 * The file, little-endian, written by nk_pcache_pack:
 *   0   8   magic "NKGEPC\0\0"
 *   8   4   schema (NK_PCACHE_SCHEMA)
 *   12  4   vendorID       ) VkPhysicalDeviceProperties of the device that made it
 *   16  4   deviceID       )
 *   20  4   driverVersion  )
 *   24  16  pipelineCacheUUID
 *   40  8   shader set: FNV-1a 64 of the renderer's SPIR-V modules
 *   48  8   size of the data that follows
 *   56  8   FNV-1a 64 of that data
 *   64  ... the data vkGetPipelineCacheData returned
 * The data must also start with the header the Vulkan specification defines
 * (VkPipelineCacheHeaderVersionOne: headerSize >= 32, headerVersion 1, then the same vendorID,
 * deviceID and pipelineCacheUUID), which nk_pcache_check checks too, as the specification
 * requires before data is handed back to vkCreatePipelineCache.
 *
 * Never trusted: a missing, short, truncated, oversized, corrupt or foreign file, or one made by
 * another device, driver or shader set, is refused with one reason and never reaches the
 * driver. The run then starts an empty cache; nothing here stops a run.
 *
 * The data is compile results only: it holds no game bytes and changes no rendered pixel. It
 * lives in the per-user cache folder, never in a package or the repository.
 */
#ifndef NK_PIPELINE_CACHE_H
#define NK_PIPELINE_CACHE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../core/nk_platform.h"

#define NK_PCACHE_SCHEMA      1u
#define NK_PCACHE_HEADER      64u
#define NK_PCACHE_MAX_DATA    (256u << 20)    /* larger is refused: no driver writes that much here */

typedef struct NkPcacheKey {
    uint32_t vendor_id, device_id, driver_version;
    uint8_t  uuid[16];          /* VkPhysicalDeviceProperties.pipelineCacheUUID */
    uint64_t shader_set;        /* nk_pcache_fnv1a64 over the renderer's SPIR-V modules */
} NkPcacheKey;

static const uint8_t k_nk_pcache_magic[8] = { 'N', 'K', 'G', 'E', 'P', 'C', 0, 0 };

static uint64_t nk_pcache_fnv1a64(uint64_t h, const void *p, size_t n) {
    const uint8_t *b = (const uint8_t *)p;
    if (!h) h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 0x100000001b3ull; }
    return h;
}

static void nk_pcache_put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void nk_pcache_put64(uint8_t *p, uint64_t v) {
    nk_pcache_put32(p, (uint32_t)v); nk_pcache_put32(p + 4, (uint32_t)(v >> 32));
}
static uint32_t nk_pcache_get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t nk_pcache_get64(const uint8_t *p) {
    return (uint64_t)nk_pcache_get32(p) | (uint64_t)nk_pcache_get32(p + 4) << 32;
}

/* The file name for a key: one file per device, so two GPUs in one computer keep one each. */
static int nk_pcache_file_name(const NkPcacheKey *k, char *out, size_t n) {
    int w = snprintf(out, n, "gegpu-%08x-%08x.vkpc", (unsigned)k->vendor_id, (unsigned)k->device_id);
    return w > 0 && (size_t)w < n;
}

/* The file image for `data`: the header above, then the data. NULL when out of memory or when
 * the data is larger than any file nk_pcache_check accepts. The caller frees it. */
static uint8_t *nk_pcache_pack(const NkPcacheKey *k, const void *data, size_t n, size_t *out_n) {
    if (n > NK_PCACHE_MAX_DATA) return NULL;
    uint8_t *buf = (uint8_t *)malloc(NK_PCACHE_HEADER + n);
    if (!buf) return NULL;
    memcpy(buf, k_nk_pcache_magic, 8);
    nk_pcache_put32(buf + 8, NK_PCACHE_SCHEMA);
    nk_pcache_put32(buf + 12, k->vendor_id);
    nk_pcache_put32(buf + 16, k->device_id);
    nk_pcache_put32(buf + 20, k->driver_version);
    memcpy(buf + 24, k->uuid, 16);
    nk_pcache_put64(buf + 40, k->shader_set);
    nk_pcache_put64(buf + 48, (uint64_t)n);
    nk_pcache_put64(buf + 56, nk_pcache_fnv1a64(0, data, n));
    if (n) memcpy(buf + NK_PCACHE_HEADER, data, n);
    *out_n = NK_PCACHE_HEADER + n;
    return buf;
}

/* Checks a whole file image against the key of this run. 1: *data and *data_n are the Vulkan
 * cache data inside buf, safe to hand to vkCreatePipelineCache. 0: *why names the first check
 * that failed, in words for one stderr line. */
static int nk_pcache_check(const uint8_t *buf, size_t n, const NkPcacheKey *k,
                           const uint8_t **data, size_t *data_n, const char **why) {
    *data = NULL; *data_n = 0;
    if (n < NK_PCACHE_HEADER) { *why = "shorter than its header"; return 0; }
    if (memcmp(buf, k_nk_pcache_magic, 8) != 0) { *why = "not a GE pipeline cache"; return 0; }
    if (nk_pcache_get32(buf + 8) != NK_PCACHE_SCHEMA) { *why = "another schema version"; return 0; }
    if (nk_pcache_get32(buf + 12) != k->vendor_id || nk_pcache_get32(buf + 16) != k->device_id) {
        *why = "made by another GPU"; return 0;
    }
    if (nk_pcache_get32(buf + 20) != k->driver_version) { *why = "made by another driver version"; return 0; }
    if (memcmp(buf + 24, k->uuid, 16) != 0) { *why = "another pipeline cache UUID"; return 0; }
    if (nk_pcache_get64(buf + 40) != k->shader_set) { *why = "made for other shaders"; return 0; }
    const uint64_t size = nk_pcache_get64(buf + 48);
    if (size > NK_PCACHE_MAX_DATA) { *why = "larger than any cache this build writes"; return 0; }
    if (n - NK_PCACHE_HEADER < size) { *why = "truncated"; return 0; }
    if (n - NK_PCACHE_HEADER > size) { *why = "longer than its header says"; return 0; }
    const uint8_t *d = buf + NK_PCACHE_HEADER;
    if (nk_pcache_fnv1a64(0, d, (size_t)size) != nk_pcache_get64(buf + 56)) { *why = "checksum mismatch"; return 0; }
    /* VkPipelineCacheHeaderVersionOne */
    if (size < 32) { *why = "the driver's header is missing"; return 0; }
    const uint32_t hsize = nk_pcache_get32(d), hver = nk_pcache_get32(d + 4);
    if (hsize < 32 || hsize > size || hver != 1u) { *why = "the driver's header is not version one"; return 0; }
    if (nk_pcache_get32(d + 8) != k->vendor_id || nk_pcache_get32(d + 12) != k->device_id ||
        memcmp(d + 16, k->uuid, 16) != 0) {
        *why = "the driver's header names another device"; return 0;
    }
    *data = d; *data_n = (size_t)size;
    return 1;
}

/* Reads a whole file. 1: *buf (malloc'd) and *n. 0: *why; "none yet" when there is no file. */
static int nk_pcache_read_file(const char *path, uint8_t **buf, size_t *n, const char **why) {
    *buf = NULL; *n = 0;
    if (nk_platform_path_missing(path)) { *why = "none yet"; return 0; }
    FILE *f = nk_fopen_utf8(path, "rb");
    if (!f) { *why = "could not be opened"; return 0; }
    uint8_t *b = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            if (cap >= NK_PCACHE_HEADER + NK_PCACHE_MAX_DATA) { *why = "larger than any cache this build writes"; break; }
            size_t nc = cap ? cap * 2 : 65536u;
            uint8_t *nb = (uint8_t *)realloc(b, nc);
            if (!nb) { *why = "out of memory"; break; }
            b = nb; cap = nc;
        }
        size_t got = fread(b + len, 1, cap - len, f);
        len += got;
        if (got == 0) {
            if (ferror(f)) { *why = "could not be read"; break; }
            fclose(f);
            *buf = b; *n = len;
            return 1;
        }
    }
    fclose(f);
    free(b);
    return 0;
}

/* SR_USER_DATA_ROOT, the data root the launcher resolved for this run (portable mode and
 * --user-data-root included), in UTF-8. 1: *out holds it. 0: unset or empty. -1: set but not
 * usable (not absolute, too long, or not valid UTF-16), which the caller must not replace with
 * another folder. Win32 reads the wide environment, so a profile folder with non-ASCII
 * characters arrives intact rather than through the ANSI code page. */
static int nk_pcache_user_data_root(char *out, size_t n) {
    if (n < 4) return -1;
#ifdef _WIN32
    const wchar_t *w = _wgetenv(L"SR_USER_DATA_ROOT");
    if (!w || !w[0]) return 0;
    size_t o = 0;
    for (size_t i = 0; w[i]; i++) {
        uint32_t c = (uint16_t)w[i];
        if (c >= 0xD800u && c <= 0xDBFFu) {
            const uint32_t lo = (uint16_t)w[i + 1];
            if (lo < 0xDC00u || lo > 0xDFFFu) return -1;
            c = 0x10000u + ((c - 0xD800u) << 10) + (lo - 0xDC00u);
            i++;
        } else if (c >= 0xDC00u && c <= 0xDFFFu) {
            return -1;
        }
        uint8_t b[4];
        size_t k;
        if (c < 0x80u) { b[0] = (uint8_t)c; k = 1; }
        else if (c < 0x800u) { b[0] = (uint8_t)(0xC0u | c >> 6); b[1] = (uint8_t)(0x80u | (c & 0x3Fu)); k = 2; }
        else if (c < 0x10000u) {
            b[0] = (uint8_t)(0xE0u | c >> 12); b[1] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu));
            b[2] = (uint8_t)(0x80u | (c & 0x3Fu)); k = 3;
        } else {
            b[0] = (uint8_t)(0xF0u | c >> 18); b[1] = (uint8_t)(0x80u | ((c >> 12) & 0x3Fu));
            b[2] = (uint8_t)(0x80u | ((c >> 6) & 0x3Fu)); b[3] = (uint8_t)(0x80u | (c & 0x3Fu)); k = 4;
        }
        if (o + k >= n) return -1;
        memcpy(out + o, b, k);
        o += k;
    }
    out[o] = 0;
    const int absolute = (((out[0] | 0x20) >= 'a' && (out[0] | 0x20) <= 'z') && out[1] == ':' &&
                          (out[2] == '\\' || out[2] == '/')) ||
                         (out[0] == '\\' && out[1] == '\\');
#else
    const char *v = getenv("SR_USER_DATA_ROOT");
    if (!v || !v[0]) return 0;
    if (strlen(v) >= n) return -1;
    memcpy(out, v, strlen(v) + 1);
    const int absolute = out[0] == '/';
#endif
    return absolute ? 1 : -1;
}

/* Writes a whole file through `path`.tmp and a rename, so a crash or a full disk never leaves a
 * half-written cache under the real name. 0: *why. */
static int nk_pcache_write_file(const char *path, const uint8_t *buf, size_t n, const char **why) {
    char tmp[4096];
    int w = snprintf(tmp, sizeof tmp, "%s.tmp", path);
    if (w <= 0 || (size_t)w >= sizeof tmp) { *why = "path too long"; return 0; }
    FILE *f = nk_fopen_utf8(tmp, "wb");
    if (!f) { *why = "could not create the file"; return 0; }
    int ok = fwrite(buf, 1, n, f) == n;
    ok = nk_platform_sync_file(f) && ok;
    ok = fclose(f) == 0 && ok;
    if (!ok) { (void)nk_remove_utf8(tmp); *why = "could not write the file"; return 0; }
    if (nk_rename_utf8(tmp, path) != 0) { (void)nk_remove_utf8(tmp); *why = "could not replace the file"; return 0; }
    return 1;
}

#endif
