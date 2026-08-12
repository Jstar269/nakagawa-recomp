// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#ifndef SR_GE_CAPTURE_H
#define SR_GE_CAPTURE_H

#include "ge_shared.h"

#include <stddef.h>
#include <stdint.h>

#define GE_CAPTURE_PAGE_SIZE 4096u
#define GE_CAPTURE_VRAM_BASE 0x04000000u
#define GE_CAPTURE_VRAM_SIZE 0x00200000u
#define GE_CAPTURE_ZBUF_WORDS (512u * 272u)

typedef struct GeCapturePage {
    uint32_t addr;
    uint8_t data[GE_CAPTURE_PAGE_SIZE];
} GeCapturePage;

typedef struct GeCaptureFixture {
    uint32_t frame;
    uint32_t *list_addrs;
    uint32_t list_count;
    GeState initial_state;
    GeState final_state;
    uint16_t *initial_zbuf;
    uint8_t *initial_vram;
    GeCapturePage *pages;
    uint32_t page_count;
} GeCaptureFixture;

/* Hot GE memory accessors branch on this directly, avoiding a function call when capture is
 * inactive. Only ge_capture_begin/abort may change it. */
extern int g_ge_capture_active;

/* Capture is scoped to one GE frame. The caller materializes any GPU-only target/depth state
 * before begin, records each list, then routes GE memory access through note_memory. VRAM and
 * the software depth buffer are copied eagerly; other guest memory is saved by first-touched
 * 4 KiB page, preserving list/vertex/index/texture/CLUT bytes actually consumed. */
/* Begin at the exact command address of the invocation that crossed into the
 * armed frame.  This may be a fresh list or a resumed stall PC; either is a
 * complete replay start when paired with the captured GE state and memory. */
int  ge_capture_begin(const char *path, uint32_t frame, uint32_t start_list_addr,
                      const GeState *state, const uint16_t *zbuf);
int  ge_capture_add_list(uint32_t list_addr);
void ge_capture_note_memory(uint32_t addr, uint32_t bytes);
int  ge_capture_end(const GeState *final_state);
void ge_capture_abort(void);
int  ge_capture_active(void);

int  ge_capture_load(const char *path, GeCaptureFixture *out);
int  ge_capture_apply(const GeCaptureFixture *fixture, GeState *state, uint16_t *zbuf);
void ge_capture_free(GeCaptureFixture *fixture);

#endif
