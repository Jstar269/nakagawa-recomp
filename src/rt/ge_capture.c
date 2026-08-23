// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "ge_capture.h"
#include "recomp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GE_CAPTURE_VERSION 2u
#define GE_CAPTURE_ARENA_SIZE 0x0c000000u
#define GE_CAPTURE_MAX_PAGES (GE_CAPTURE_ARENA_SIZE / GE_CAPTURE_PAGE_SIZE)

typedef struct GeCaptureHeader {
    char magic[8];
    uint32_t version;
    uint32_t header_size;
    uint32_t ge_state_size;
    uint32_t page_size;
    uint32_t frame;
    uint32_t list_count;
    uint32_t zbuf_bytes;
    uint32_t vram_base;
    uint32_t vram_bytes;
    uint32_t page_count;
    uint32_t reserved[5];
} GeCaptureHeader;

typedef struct CaptureState {
    int active;
    char path[1024];
    GeCaptureFixture fixture;
    uint8_t *seen;
    uint32_t page_cap;
    uint32_t list_cap;
} CaptureState;

static CaptureState s_capture;
int g_ge_capture_active;
static const char k_magic[8] = { 'N', 'G', 'E', 'F', '0', '0', '0', '2' };

static int write_exact(FILE *file, const void *data, size_t size) {
    return size == 0 || fwrite(data, 1, size, file) == size;
}

static int read_exact(FILE *file, void *data, size_t size) {
    return size == 0 || fread(data, 1, size, file) == size;
}

void ge_capture_free(GeCaptureFixture *fixture) {
    if (!fixture) return;
    free(fixture->initial_zbuf);
    free(fixture->initial_vram);
    free(fixture->list_addrs);
    free(fixture->pages);
    memset(fixture, 0, sizeof(*fixture));
}

void ge_capture_abort(void) {
    ge_capture_free(&s_capture.fixture);
    free(s_capture.seen);
    memset(&s_capture, 0, sizeof(s_capture));
    g_ge_capture_active = 0;
}

int ge_capture_active(void) { return g_ge_capture_active; }

int ge_capture_begin(const char *path, uint32_t frame, uint32_t start_list_addr,
                     const GeState *state, const uint16_t *zbuf) {
    if (!path || !path[0] || !state || !zbuf || !g_mem || s_capture.active ||
        (start_list_addr & 3u) != 0 || SR_PHYS(start_list_addr) >= GE_CAPTURE_ARENA_SIZE ||
        !sr_guest_span_readable(GE_CAPTURE_VRAM_BASE, GE_CAPTURE_VRAM_SIZE))
        return 0;
    memset(&s_capture, 0, sizeof(s_capture));
    if (strlen(path) >= sizeof(s_capture.path)) return 0;
    memcpy(s_capture.path, path, strlen(path) + 1);
    GeCaptureFixture *f = &s_capture.fixture;
    f->frame = frame;
    f->initial_state = *state;
    f->initial_zbuf = (uint16_t *)malloc(GE_CAPTURE_ZBUF_WORDS * sizeof(uint16_t));
    f->initial_vram = (uint8_t *)malloc(GE_CAPTURE_VRAM_SIZE);
    s_capture.seen = (uint8_t *)calloc(GE_CAPTURE_MAX_PAGES, 1);
    if (!f->initial_zbuf || !f->initial_vram || !s_capture.seen) {
        ge_capture_abort();
        return 0;
    }
    memcpy(f->initial_zbuf, zbuf, GE_CAPTURE_ZBUF_WORDS * sizeof(uint16_t));
    memcpy(f->initial_vram, SR_HOST(GE_CAPTURE_VRAM_BASE), GE_CAPTURE_VRAM_SIZE);
    s_capture.active = 1;
    g_ge_capture_active = 1;
    if (!ge_capture_add_list(start_list_addr)) {
        ge_capture_abort();
        return 0;
    }
    return 1;
}

int ge_capture_add_list(uint32_t list_addr) {
    if (!s_capture.active) return 0;
    GeCaptureFixture *f = &s_capture.fixture;
    if (f->list_count == s_capture.list_cap) {
        uint32_t next = s_capture.list_cap ? s_capture.list_cap * 2u : 16u;
        uint32_t *lists = (uint32_t *)realloc(f->list_addrs, (size_t)next * sizeof(*lists));
        if (!lists) return 0;
        f->list_addrs = lists;
        s_capture.list_cap = next;
    }
    f->list_addrs[f->list_count++] = list_addr;
    return 1;
}

static int append_page(uint32_t page_addr) {
    if (!sr_guest_span_readable(page_addr, GE_CAPTURE_PAGE_SIZE)) return 0;
    GeCaptureFixture *f = &s_capture.fixture;
    if (f->page_count == s_capture.page_cap) {
        uint32_t next = s_capture.page_cap ? s_capture.page_cap * 2u : 64u;
        GeCapturePage *pages = (GeCapturePage *)realloc(f->pages, (size_t)next * sizeof(*pages));
        if (!pages) return 0;
        f->pages = pages;
        s_capture.page_cap = next;
    }
    GeCapturePage *page = &f->pages[f->page_count++];
    page->addr = page_addr;
    memcpy(page->data, SR_HOST(page_addr), GE_CAPTURE_PAGE_SIZE);
    return 1;
}

void ge_capture_note_memory(uint32_t addr, uint32_t bytes) {
    if (!s_capture.active || bytes == 0) return;
    uint64_t first = SR_PHYS(addr);
    uint64_t last = first + (uint64_t)bytes - 1u;
    if (first >= GE_CAPTURE_ARENA_SIZE || last >= GE_CAPTURE_ARENA_SIZE) return;
    uint32_t first_page = (uint32_t)first / GE_CAPTURE_PAGE_SIZE;
    uint32_t last_page = (uint32_t)last / GE_CAPTURE_PAGE_SIZE;
    for (uint32_t index = first_page; index <= last_page; index++) {
        uint32_t page_addr = index * GE_CAPTURE_PAGE_SIZE;
        if (page_addr >= GE_CAPTURE_VRAM_BASE &&
            page_addr < GE_CAPTURE_VRAM_BASE + GE_CAPTURE_VRAM_SIZE)
            continue;
        if (s_capture.seen[index]) continue;
        if (!append_page(page_addr)) {
            fprintf(stderr, "GE_CAPTURE: out of memory while saving page 0x%08x\n", page_addr);
            ge_capture_abort();
            return;
        }
        s_capture.seen[index] = 1;
    }
}

int ge_capture_end(const GeState *final_state) {
    if (!s_capture.active || !final_state || s_capture.fixture.list_count == 0) return 0;
    GeCaptureFixture *f = &s_capture.fixture;
    f->final_state = *final_state;
    GeCaptureHeader h;
    memset(&h, 0, sizeof(h));
    memcpy(h.magic, k_magic, sizeof(h.magic));
    h.version = GE_CAPTURE_VERSION;
    h.header_size = (uint32_t)sizeof(h);
    h.ge_state_size = (uint32_t)sizeof(GeState);
    h.page_size = GE_CAPTURE_PAGE_SIZE;
    h.frame = f->frame;
    h.list_count = f->list_count;
    h.zbuf_bytes = GE_CAPTURE_ZBUF_WORDS * (uint32_t)sizeof(uint16_t);
    h.vram_base = GE_CAPTURE_VRAM_BASE;
    h.vram_bytes = GE_CAPTURE_VRAM_SIZE;
    h.page_count = f->page_count;

    FILE *file = fopen(s_capture.path, "wb");
    int ok = file != NULL;
    if (ok) ok = write_exact(file, &h, sizeof(h));
    if (ok) ok = write_exact(file, &f->initial_state, sizeof(f->initial_state));
    if (ok) ok = write_exact(file, &f->final_state, sizeof(f->final_state));
    if (ok) ok = write_exact(file, f->list_addrs, (size_t)h.list_count * sizeof(uint32_t));
    if (ok) ok = write_exact(file, f->initial_zbuf, h.zbuf_bytes);
    if (ok) ok = write_exact(file, f->initial_vram, h.vram_bytes);
    for (uint32_t i = 0; ok && i < f->page_count; i++) {
        ok = write_exact(file, &f->pages[i].addr, sizeof(f->pages[i].addr));
        if (ok) ok = write_exact(file, f->pages[i].data, GE_CAPTURE_PAGE_SIZE);
    }
    if (file && fclose(file) != 0) ok = 0;
    if (ok) {
        fprintf(stderr, "GE_CAPTURE: wrote %s frame=%u lists=%u pages=%u bytes=%llu\n",
                s_capture.path, f->frame, f->list_count, f->page_count,
                (unsigned long long)(sizeof(h) + 2u * sizeof(GeState) +
                                     (uint64_t)h.list_count * sizeof(uint32_t) + h.zbuf_bytes +
                                     h.vram_bytes + (uint64_t)f->page_count *
                                     (sizeof(uint32_t) + GE_CAPTURE_PAGE_SIZE)));
    } else {
        fprintf(stderr, "GE_CAPTURE: failed to write %s\n", s_capture.path);
    }
    ge_capture_abort();
    return ok;
}

int ge_capture_load(const char *path, GeCaptureFixture *out) {
    if (!path || !out) return 0;
    memset(out, 0, sizeof(*out));
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    GeCaptureHeader h;
    int ok = read_exact(file, &h, sizeof(h));
    ok = ok && memcmp(h.magic, k_magic, sizeof(h.magic)) == 0;
    ok = ok && h.version == GE_CAPTURE_VERSION && h.header_size == sizeof(h);
    ok = ok && h.ge_state_size == sizeof(GeState) && h.page_size == GE_CAPTURE_PAGE_SIZE;
    ok = ok && h.zbuf_bytes == GE_CAPTURE_ZBUF_WORDS * sizeof(uint16_t);
    ok = ok && h.vram_base == GE_CAPTURE_VRAM_BASE && h.vram_bytes == GE_CAPTURE_VRAM_SIZE;
    ok = ok && h.page_count <= GE_CAPTURE_MAX_PAGES;
    ok = ok && h.list_count > 0 && h.list_count <= (1u << 20);
    if (!ok) { fclose(file); return 0; }

    out->frame = h.frame;
    out->list_count = h.list_count;
    out->page_count = h.page_count;
    out->list_addrs = (uint32_t *)malloc((size_t)h.list_count * sizeof(uint32_t));
    out->initial_zbuf = (uint16_t *)malloc(h.zbuf_bytes);
    out->initial_vram = (uint8_t *)malloc(h.vram_bytes);
    if (h.page_count)
        out->pages = (GeCapturePage *)calloc(h.page_count, sizeof(*out->pages));
    if (!out->list_addrs || !out->initial_zbuf || !out->initial_vram ||
        (h.page_count && !out->pages)) ok = 0;
    if (ok) ok = read_exact(file, &out->initial_state, sizeof(out->initial_state));
    if (ok) ok = read_exact(file, &out->final_state, sizeof(out->final_state));
    if (ok) ok = read_exact(file, out->list_addrs, (size_t)h.list_count * sizeof(uint32_t));
    for (uint32_t i = 0; ok && i < h.list_count; i++)
        ok = (out->list_addrs[i] & 3u) == 0 && SR_PHYS(out->list_addrs[i]) < GE_CAPTURE_ARENA_SIZE;
    if (ok) ok = read_exact(file, out->initial_zbuf, h.zbuf_bytes);
    if (ok) ok = read_exact(file, out->initial_vram, h.vram_bytes);
    uint8_t *seen = ok ? (uint8_t *)calloc(GE_CAPTURE_MAX_PAGES, 1) : NULL;
    if (ok && !seen) ok = 0;
    for (uint32_t i = 0; ok && i < h.page_count; i++) {
        GeCapturePage *page = &out->pages[i];
        ok = read_exact(file, &page->addr, sizeof(page->addr));
        uint32_t index = page->addr / GE_CAPTURE_PAGE_SIZE;
        ok = ok && (page->addr % GE_CAPTURE_PAGE_SIZE) == 0;
        ok = ok && page->addr < GE_CAPTURE_ARENA_SIZE && index < GE_CAPTURE_MAX_PAGES;
        ok = ok && !(page->addr >= GE_CAPTURE_VRAM_BASE &&
                     page->addr < GE_CAPTURE_VRAM_BASE + GE_CAPTURE_VRAM_SIZE);
        ok = ok && !seen[index];
        if (ok) seen[index] = 1;
        if (ok) ok = read_exact(file, page->data, GE_CAPTURE_PAGE_SIZE);
    }
    free(seen);
    if (ok) ok = fgetc(file) == EOF;
    fclose(file);
    if (!ok) ge_capture_free(out);
    return ok;
}

int ge_capture_apply(const GeCaptureFixture *fixture, GeState *state, uint16_t *zbuf) {
    if (!fixture || !state || !zbuf || !g_mem || !fixture->initial_zbuf ||
        !fixture->initial_vram || (fixture->page_count && !fixture->pages) ||
        !sr_guest_span_writable(GE_CAPTURE_VRAM_BASE, GE_CAPTURE_VRAM_SIZE)) return 0;
    memcpy(SR_HOST(GE_CAPTURE_VRAM_BASE), fixture->initial_vram, GE_CAPTURE_VRAM_SIZE);
    for (uint32_t i = 0; i < fixture->page_count; i++) {
        uint32_t addr = fixture->pages[i].addr;
        if ((addr % GE_CAPTURE_PAGE_SIZE) != 0 ||
            !sr_guest_span_writable(addr, GE_CAPTURE_PAGE_SIZE)) return 0;
        memcpy(SR_HOST(addr), fixture->pages[i].data, GE_CAPTURE_PAGE_SIZE);
    }
    *state = fixture->initial_state;
    memcpy(zbuf, fixture->initial_zbuf, GE_CAPTURE_ZBUF_WORDS * sizeof(uint16_t));
    return 1;
}
