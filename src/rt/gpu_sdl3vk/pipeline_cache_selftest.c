/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * pipeline_cache_selftest.c - the checks pipeline_cache.h makes before a pipeline cache file
 * reaches the driver (#2149), with no GPU. Each cell builds a good file image for a synthetic
 * key, breaks one thing, and requires nk_pcache_check to refuse it with that thing's reason.
 * Then the file round trip: write through a temporary name and a rename, read back, replace,
 * and a missing file reads as "none yet".
 *
 * usage: pipeline_cache_selftest <scratch folder>   (it creates and removes its own files there)
 */
#include "pipeline_cache.h"

static int s_fail = 0;

static void expect_refused(const char *name, const uint8_t *buf, size_t n, const NkPcacheKey *k, const char *want) {
    const uint8_t *data = (const uint8_t *)1;
    size_t dn = 1;
    const char *why = "(none)";
    int ok = nk_pcache_check(buf, n, k, &data, &dn, &why);
    if (ok || strcmp(why, want) != 0 || data != NULL || dn != 0) {
        fprintf(stderr, "pipeline cache selftest: FAIL %s: %s (%s), want refused (%s)\n", name,
                ok ? "accepted" : "refused", ok ? "-" : why, want);
        s_fail++;
    } else {
        printf("pipeline cache selftest: PASS %-28s refused: %s\n", name, why);
    }
}

/* A Vulkan cache image as a driver writes it: VkPipelineCacheHeaderVersionOne, then payload. */
static uint8_t *driver_data(const NkPcacheKey *k, size_t payload, size_t *n) {
    *n = 32 + payload;
    uint8_t *d = (uint8_t *)malloc(*n);
    if (!d) exit(2);
    nk_pcache_put32(d, 32);
    nk_pcache_put32(d + 4, 1);
    nk_pcache_put32(d + 8, k->vendor_id);
    nk_pcache_put32(d + 12, k->device_id);
    memcpy(d + 16, k->uuid, 16);
    for (size_t i = 0; i < payload; i++) d[32 + i] = (uint8_t)(i * 37u + 11u);
    return d;
}

/* A copy of a good file image, with `extra` bytes after it for the trailing-byte cell. A cell
 * that rewrites the driver's data calls fix_sum, so it reaches the check it is about instead of
 * stopping at the checksum. */
static uint8_t *variant(const uint8_t *good, size_t n, size_t extra) {
    uint8_t *v = (uint8_t *)malloc(n + extra + 1);
    if (!v) exit(2);
    memcpy(v, good, n);
    memset(v + n, 0x5A, extra + 1);
    return v;
}
static void fix_sum(uint8_t *v, size_t n) {
    nk_pcache_put64(v + 56, nk_pcache_fnv1a64(0, v + NK_PCACHE_HEADER, n - NK_PCACHE_HEADER));
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: pipeline_cache_selftest <scratch folder>\n");
        return 2;
    }
    NkPcacheKey key = { 0x10DEu, 0x2206u, 0x8E5A4000u, { 0 }, 0x0123456789ABCDEFull };
    for (int i = 0; i < 16; i++) key.uuid[i] = (uint8_t)(0xA0 + i);

    size_t dn = 0;
    uint8_t *drv = driver_data(&key, 1000, &dn);
    size_t n = 0;
    uint8_t *good = nk_pcache_pack(&key, drv, dn, &n);
    if (!good || n != NK_PCACHE_HEADER + dn) { fprintf(stderr, "pipeline cache selftest: FAIL pack\n"); return 1; }

    /* The good image is accepted, and its data is the driver's bytes, in place. */
    {
        const uint8_t *data = NULL;
        size_t got = 0;
        const char *why = NULL;
        if (!nk_pcache_check(good, n, &key, &data, &got, &why) || data != good + NK_PCACHE_HEADER || got != dn ||
            memcmp(data, drv, dn) != 0) {
            fprintf(stderr, "pipeline cache selftest: FAIL good file refused (%s)\n", why ? why : "-");
            s_fail++;
        } else {
            printf("pipeline cache selftest: PASS %-28s accepted, %zu bytes of driver data\n", "good", got);
        }
    }

    /* Another device, driver or shader set: the key of this run differs from the file's. */
    NkPcacheKey k2;
    k2 = key; k2.vendor_id ^= 1u;      expect_refused("another vendor", good, n, &k2, "made by another GPU");
    k2 = key; k2.device_id ^= 1u;      expect_refused("another device", good, n, &k2, "made by another GPU");
    k2 = key; k2.driver_version += 1u; expect_refused("another driver", good, n, &k2, "made by another driver version");
    k2 = key; k2.uuid[15] ^= 1u;       expect_refused("another cache UUID", good, n, &k2, "another pipeline cache UUID");
    k2 = key; k2.shader_set ^= 1u;     expect_refused("other shaders", good, n, &k2, "made for other shaders");

    /* Foreign, short and truncated files. */
    {
        static const uint8_t png[NK_PCACHE_HEADER + 8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
        expect_refused("foreign file", png, sizeof png, &key, "not a GE pipeline cache");
        expect_refused("empty file", good, 0, &key, "shorter than its header");
        expect_refused("short header", good, NK_PCACHE_HEADER - 1, &key, "shorter than its header");
        expect_refused("truncated data", good, n - 1, &key, "truncated");
        expect_refused("header only", good, NK_PCACHE_HEADER, &key, "truncated");
        uint8_t *v = variant(good, n, 1);
        expect_refused("trailing byte", v, n + 1, &key, "longer than its header says");
        free(v);
        v = variant(good, n, 0);
        nk_pcache_put32(v + 8, NK_PCACHE_SCHEMA + 1u);
        expect_refused("another schema", v, n, &key, "another schema version");
        memcpy(v, good, n);
        nk_pcache_put64(v + 48, (uint64_t)NK_PCACHE_MAX_DATA + 1u);
        expect_refused("oversized size field", v, n, &key, "larger than any cache this build writes");
        memcpy(v, good, n);
        v[NK_PCACHE_HEADER + 500] ^= 0x40u;
        expect_refused("corrupt data", v, n, &key, "checksum mismatch");
        free(v);
    }

    /* The driver's own header inside the data (the checksum is fixed, so these reach it). */
    {
        uint8_t *v = variant(good, n, 0);
        nk_pcache_put32(v + NK_PCACHE_HEADER + 4, 2u); fix_sum(v, n);
        expect_refused("driver header version 2", v, n, &key, "the driver's header is not version one");
        memcpy(v, good, n);
        nk_pcache_put32(v + NK_PCACHE_HEADER, 16u); fix_sum(v, n);
        expect_refused("driver header size 16", v, n, &key, "the driver's header is not version one");
        memcpy(v, good, n);
        nk_pcache_put32(v + NK_PCACHE_HEADER, (uint32_t)dn + 1u); fix_sum(v, n);
        expect_refused("driver header past the data", v, n, &key, "the driver's header is not version one");
        memcpy(v, good, n);
        nk_pcache_put32(v + NK_PCACHE_HEADER + 8, key.vendor_id ^ 1u); fix_sum(v, n);
        expect_refused("driver header vendor", v, n, &key, "the driver's header names another device");
        memcpy(v, good, n);
        nk_pcache_put32(v + NK_PCACHE_HEADER + 12, key.device_id ^ 1u); fix_sum(v, n);
        expect_refused("driver header device", v, n, &key, "the driver's header names another device");
        memcpy(v, good, n);
        v[NK_PCACHE_HEADER + 31] ^= 1u; fix_sum(v, n);
        expect_refused("driver header UUID", v, n, &key, "the driver's header names another device");
        free(v);
        size_t sn = 0;
        uint8_t *small = nk_pcache_pack(&key, drv, 31, &sn);
        expect_refused("driver header missing", small, sn, &key, "the driver's header is missing");
        free(small);
        if (nk_pcache_pack(&key, drv, (size_t)NK_PCACHE_MAX_DATA + 1u, &sn) != NULL) {
            fprintf(stderr, "pipeline cache selftest: FAIL pack accepted data past the limit\n");
            s_fail++;
        }
    }

    /* The file round trip, in the scratch folder. */
    {
        char path[4096], tmp[4200], name[64];
        if (!nk_platform_mkdir_p(argv[1]) || !nk_pcache_file_name(&key, name, sizeof name)) {
            fprintf(stderr, "pipeline cache selftest: FAIL scratch folder %s\n", argv[1]);
            return 1;
        }
        snprintf(path, sizeof path, "%s%c%s", argv[1], nk_platform_path_separator(), name);
        snprintf(tmp, sizeof tmp, "%s.tmp", path);
        (void)nk_remove_utf8(path);
        (void)nk_remove_utf8(tmp);
        uint8_t *buf = NULL;
        size_t got = 0;
        const char *why = NULL;
        int ok = 1;
        if (nk_pcache_read_file(path, &buf, &got, &why) || strcmp(why, "none yet") != 0) {
            fprintf(stderr, "pipeline cache selftest: FAIL missing file read as %s\n", buf ? "a file" : why);
            ok = 0;
        }
        free(buf); buf = NULL;
        ok = ok && nk_pcache_write_file(path, good, n, &why);
        ok = ok && nk_pcache_read_file(path, &buf, &got, &why) && got == n && memcmp(buf, good, n) == 0;
        free(buf); buf = NULL;
        /* Replacing an existing file goes through the same rename. */
        size_t n2 = 0, dn2 = 0;
        uint8_t *drv2 = driver_data(&key, 70000, &dn2);   /* past the first read's 64 KiB step */
        uint8_t *good2 = nk_pcache_pack(&key, drv2, dn2, &n2);
        ok = ok && good2 && nk_pcache_write_file(path, good2, n2, &why);
        ok = ok && nk_pcache_read_file(path, &buf, &got, &why) && got == n2 && memcmp(buf, good2, n2) == 0;
        const uint8_t *data = NULL;
        size_t dgot = 0;
        ok = ok && nk_pcache_check(buf, got, &key, &data, &dgot, &why) && dgot == dn2;
        if (ok && !nk_platform_path_missing(tmp)) { why = "the temporary file was left behind"; ok = 0; }
        if (!ok) {
            fprintf(stderr, "pipeline cache selftest: FAIL file round trip (%s)\n", why ? why : "content differs");
            s_fail++;
        } else {
            printf("pipeline cache selftest: PASS %-28s written, replaced, read back, no temporary file left\n",
                   "file round trip");
        }
        free(buf); free(good2); free(drv2);
        (void)nk_remove_utf8(path);
    }

    /* SR_USER_DATA_ROOT, the cache's data root: unset is "none", a relative or oversized value
     * is refused (never replaced by another folder), and an absolute one arrives as UTF-8. */
    {
        char root[64];
#ifdef _WIN32
        const char *abs_utf8 = "D:\\Games\\Jos\xc3\xa9\\NK \xf0\x9f\x8e\xae";   /* a non-BMP code point too */
        int ok = _wputenv_s(L"SR_USER_DATA_ROOT", L"") == 0 && nk_pcache_user_data_root(root, sizeof root) == 0;
        ok = ok && _wputenv_s(L"SR_USER_DATA_ROOT", L"data\\nk") == 0 && nk_pcache_user_data_root(root, sizeof root) == -1;
        ok = ok && _wputenv_s(L"SR_USER_DATA_ROOT", L"D:\\Games\\Jos\u00e9\\NK \U0001F3AE") == 0 &&
             nk_pcache_user_data_root(root, sizeof root) == 1 && strcmp(root, abs_utf8) == 0;
        ok = ok && nk_pcache_user_data_root(root, 12) == -1;
        (void)_wputenv_s(L"SR_USER_DATA_ROOT", L"");
#else
        int ok = unsetenv("SR_USER_DATA_ROOT") == 0 && nk_pcache_user_data_root(root, sizeof root) == 0;
        ok = ok && setenv("SR_USER_DATA_ROOT", "data/nk", 1) == 0 && nk_pcache_user_data_root(root, sizeof root) == -1;
        ok = ok && setenv("SR_USER_DATA_ROOT", "/srv/jos\xc3\xa9/nk", 1) == 0 &&
             nk_pcache_user_data_root(root, sizeof root) == 1 && strcmp(root, "/srv/jos\xc3\xa9/nk") == 0;
        ok = ok && nk_pcache_user_data_root(root, 8) == -1;
        (void)unsetenv("SR_USER_DATA_ROOT");
#endif
        if (!ok) {
            fprintf(stderr, "pipeline cache selftest: FAIL data root (SR_USER_DATA_ROOT)\n");
            s_fail++;
        } else {
            printf("pipeline cache selftest: PASS %-28s unset, relative refused, absolute as UTF-8, too long refused\n",
                   "data root");
        }
    }

    free(good);
    free(drv);
    if (s_fail) {
        fprintf(stderr, "pipeline cache selftest: %d failure(s)\n", s_fail);
        return 1;
    }
    puts("pipeline cache selftest: OK");
    return 0;
}
