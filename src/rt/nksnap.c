// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* nksnap v1 loader and writer (see nksnap.h). */

#include "nksnap.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const struct {
    uint32_t first;
    uint32_t size;
    const char *name;
} k_regions[] = {
    { 0x00010000u, 0x00004000u, "scratchpad" },
    { 0x04000000u, 0x00200000u, "vram" },
    { 0x08000000u, 0x04000000u, "ram" },
};

const char *nksnap_status_name(NkSnapStatus status) {
    switch (status) {
    case NKSNAP_OK: return "ok";
    case NKSNAP_BAD_MAGIC: return "bad-magic";
    case NKSNAP_ABI_VERSION: return "abi-version";
    case NKSNAP_CPUSTATE_SIZE: return "cpustate-size";
    case NKSNAP_TRUNCATED: return "truncated";
    case NKSNAP_BLOCK_SIZE: return "block-size";
    case NKSNAP_BLOCK_OVERLAP: return "block-overlap";
    case NKSNAP_BLOCK_OUTSIDE_GUEST: return "block-outside-guest";
    case NKSNAP_TRAILING_BYTES: return "trailing-bytes";
    case NKSNAP_IO_ERROR: return "io-error";
    case NKSNAP_NO_MEMORY: return "out-of-memory";
    }
    return "unknown";
}

const char *nksnap_guest_region(uint32_t addr, uint32_t size) {
    for (size_t i = 0; i < sizeof k_regions / sizeof k_regions[0]; i++) {
        uint64_t first = k_regions[i].first;
        uint64_t end = first + k_regions[i].size;
        if ((uint64_t)addr >= first && (uint64_t)addr + size <= end)
            return k_regions[i].name;
    }
    return NULL;
}

static void set_detail(char *detail, size_t cap, const char *fmt, ...) {
    if (!detail || cap == 0u) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(detail, cap, fmt, ap);
    va_end(ap);
}

static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static int block_order(const void *a, const void *b) {
    const NkSnapBlock *x = (const NkSnapBlock *)a, *y = (const NkSnapBlock *)b;
    return x->addr < y->addr ? -1 : x->addr > y->addr ? 1 : 0;
}

/* Size, overlap and region checks shared by the loader and the writer. */
static NkSnapStatus check_blocks(const NkSnapBlock *blocks, uint32_t count,
                                 char *detail, size_t cap) {
    for (uint32_t i = 0; i < count; i++) {
        if (blocks[i].size == 0u || (uint64_t)blocks[i].addr + blocks[i].size > UINT64_C(0x100000000)) {
            set_detail(detail, cap, "block %u at 0x%08x size 0x%x", (unsigned)i,
                       (unsigned)blocks[i].addr, (unsigned)blocks[i].size);
            return NKSNAP_BLOCK_SIZE;
        }
    }
    if (count > 1u) {
        NkSnapBlock *sorted = (NkSnapBlock *)malloc(sizeof *sorted * count);
        if (!sorted) return NKSNAP_NO_MEMORY;
        memcpy(sorted, blocks, sizeof *sorted * count);
        qsort(sorted, count, sizeof *sorted, block_order);
        for (uint32_t i = 1; i < count; i++) {
            if ((uint64_t)sorted[i - 1u].addr + sorted[i - 1u].size > sorted[i].addr) {
                set_detail(detail, cap, "0x%08x+0x%x overlaps 0x%08x",
                           (unsigned)sorted[i - 1u].addr, (unsigned)sorted[i - 1u].size,
                           (unsigned)sorted[i].addr);
                free(sorted);
                return NKSNAP_BLOCK_OVERLAP;
            }
        }
        free(sorted);
    }
    for (uint32_t i = 0; i < count; i++) {
        if (!nksnap_guest_region(blocks[i].addr, blocks[i].size)) {
            set_detail(detail, cap, "0x%08x+0x%x", (unsigned)blocks[i].addr,
                       (unsigned)blocks[i].size);
            return NKSNAP_BLOCK_OUTSIDE_GUEST;
        }
    }
    return NKSNAP_OK;
}

NkSnapStatus nksnap_parse(const uint8_t *data, size_t len, NkSnapshot *out,
                          char *detail, size_t cap) {
    memset(out, 0, sizeof *out);
    if (detail && cap) detail[0] = '\0';
    if (len < NKSNAP_MAGIC_LEN) {
        set_detail(detail, cap, "shorter than the magic");
        return NKSNAP_TRUNCATED;
    }
    if (memcmp(data, NKSNAP_MAGIC, NKSNAP_MAGIC_LEN) != 0) return NKSNAP_BAD_MAGIC;
    size_t pos = NKSNAP_MAGIC_LEN;
#define NKSNAP_NEED(n, what) do { \
        if (len - pos < (size_t)(n)) { \
            set_detail(detail, cap, "%s at offset %zu", (what), pos); \
            status = NKSNAP_TRUNCATED; goto fail; \
        } \
    } while (0)
    NkSnapStatus status = NKSNAP_OK;
    NkSnapBlock *blocks = NULL;
    uint8_t *storage = NULL;

    NKSNAP_NEED(4u, "ABI version");
    uint32_t abi = rd32(data + pos); pos += 4u;
    if (abi != SR_CPUSTATE_ABI_VERSION) {
        set_detail(detail, cap, "file %u, runtime %u", (unsigned)abi,
                   (unsigned)SR_CPUSTATE_ABI_VERSION);
        return NKSNAP_ABI_VERSION;
    }
    NKSNAP_NEED(4u, "CpuState size");
    uint32_t cpu_size = rd32(data + pos); pos += 4u;
    if (cpu_size != (uint32_t)sizeof(CpuState)) {
        set_detail(detail, cap, "file %u, runtime %u", (unsigned)cpu_size,
                   (unsigned)sizeof(CpuState));
        return NKSNAP_CPUSTATE_SIZE;
    }
    NKSNAP_NEED(sizeof(CpuState), "CpuState");
    const size_t cpu_pos = pos;
    pos += sizeof(CpuState);
    NKSNAP_NEED(8u, "entry PC and block count");
    uint32_t entry = rd32(data + pos);
    uint32_t count = rd32(data + pos + 4u);
    pos += 8u;
    /* Every block needs at least its 8-byte header, which bounds the allocation. */
    if (count > (len - pos) / 8u) {
        set_detail(detail, cap, "%u block headers do not fit in %zu bytes", (unsigned)count,
                   len - pos);
        return NKSNAP_TRUNCATED;
    }
    storage = (uint8_t *)malloc(len ? len : 1u);
    blocks = (NkSnapBlock *)calloc(count ? count : 1u, sizeof *blocks);
    if (!storage || !blocks) { status = NKSNAP_NO_MEMORY; goto fail; }
    memcpy(storage, data, len);
    for (uint32_t i = 0; i < count; i++) {
        char what[48];
        snprintf(what, sizeof what, "block %u header", (unsigned)i);
        NKSNAP_NEED(8u, what);
        uint32_t addr = rd32(data + pos), size = rd32(data + pos + 4u);
        pos += 8u;
        if (size == 0u || (uint64_t)addr + size > UINT64_C(0x100000000)) {
            set_detail(detail, cap, "block %u at 0x%08x size 0x%x", (unsigned)i,
                       (unsigned)addr, (unsigned)size);
            status = NKSNAP_BLOCK_SIZE;
            goto fail;
        }
        snprintf(what, sizeof what, "block %u bytes", (unsigned)i);
        NKSNAP_NEED(size, what);
        blocks[i].addr = addr;
        blocks[i].size = size;
        blocks[i].bytes = storage + pos;
        pos += size;
    }
#undef NKSNAP_NEED
    if (pos != len) {
        set_detail(detail, cap, "%zu byte(s) after the last block", len - pos);
        status = NKSNAP_TRAILING_BYTES;
        goto fail;
    }
    status = check_blocks(blocks, count, detail, cap);
    if (status != NKSNAP_OK) goto fail;
    memcpy(&out->cpu, data + cpu_pos, sizeof(CpuState));
    out->entry_pc = entry;
    out->block_count = count;
    out->blocks = blocks;
    out->storage = storage;
    return NKSNAP_OK;
fail:
    free(blocks);
    free(storage);
    return status;
}

NkSnapStatus nksnap_load_file(const char *path, NkSnapshot *out, char *detail, size_t cap) {
    memset(out, 0, sizeof *out);
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_detail(detail, cap, "cannot open %s", path);
        return NKSNAP_IO_ERROR;
    }
    size_t used = 0, room = 1u << 16;
    uint8_t *buf = (uint8_t *)malloc(room);
    if (!buf) { fclose(f); return NKSNAP_NO_MEMORY; }
    for (;;) {
        if (used == room) {
            if (room > (SIZE_MAX / 2u)) { free(buf); fclose(f); return NKSNAP_NO_MEMORY; }
            uint8_t *grown = (uint8_t *)realloc(buf, room * 2u);
            if (!grown) { free(buf); fclose(f); return NKSNAP_NO_MEMORY; }
            buf = grown;
            room *= 2u;
        }
        size_t got = fread(buf + used, 1, room - used, f);
        used += got;
        if (got == 0u) break;
    }
    int failed = ferror(f);
    fclose(f);
    if (failed) {
        free(buf);
        set_detail(detail, cap, "read error on %s", path);
        return NKSNAP_IO_ERROR;
    }
    NkSnapStatus status = nksnap_parse(buf, used, out, detail, cap);
    free(buf);
    return status;
}

void nksnap_free(NkSnapshot *snapshot) {
    if (!snapshot) return;
    free(snapshot->blocks);
    free(snapshot->storage);
    memset(snapshot, 0, sizeof *snapshot);
}

NkSnapStatus nksnap_write_file(const char *path, const CpuState *cpu, uint32_t entry_pc,
                               const NkSnapBlock *blocks, uint32_t count,
                               char *detail, size_t cap) {
    if (detail && cap) detail[0] = '\0';
    NkSnapStatus status = check_blocks(blocks, count, detail, cap);
    if (status != NKSNAP_OK) return status;
    FILE *f = fopen(path, "wb");
    if (!f) {
        set_detail(detail, cap, "cannot create %s", path);
        return NKSNAP_IO_ERROR;
    }
    uint8_t word[8];
    int ok = fwrite(NKSNAP_MAGIC, 1, NKSNAP_MAGIC_LEN, f) == NKSNAP_MAGIC_LEN;
    wr32(word, SR_CPUSTATE_ABI_VERSION);
    wr32(word + 4, (uint32_t)sizeof(CpuState));
    ok = ok && fwrite(word, 1, 8, f) == 8u;
    ok = ok && fwrite(cpu, 1, sizeof(CpuState), f) == sizeof(CpuState);
    wr32(word, entry_pc);
    wr32(word + 4, count);
    ok = ok && fwrite(word, 1, 8, f) == 8u;
    for (uint32_t i = 0; ok && i < count; i++) {
        wr32(word, blocks[i].addr);
        wr32(word + 4, blocks[i].size);
        ok = fwrite(word, 1, 8, f) == 8u &&
             fwrite(blocks[i].bytes, 1, blocks[i].size, f) == blocks[i].size;
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
        set_detail(detail, cap, "write error on %s", path);
        return NKSNAP_IO_ERROR;
    }
    return NKSNAP_OK;
}
