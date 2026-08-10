// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#include "ge_capture.h"
#include "recomp.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t *g_mem;

int main(int argc, char **argv) {
    assert(argc == 2);
    uint8_t *arena = (uint8_t *)calloc(0x0c000000u, 1);
    assert(arena);
    g_mem = arena + 0x08000000u;
    GeState initial, final;
    uint16_t zbuf[GE_CAPTURE_ZBUF_WORDS];
    memset(&initial, 0, sizeof(initial));
    memset(&final, 0, sizeof(final));
    for (uint32_t i = 0; i < GE_CAPTURE_ZBUF_WORDS; i++) zbuf[i] = (uint16_t)(i ^ 0x5a5au);
    initial.fbp = GE_CAPTURE_VRAM_BASE + 0x4000u;
    initial.fbw = 512;
    initial.fbfmt = 2;
    final = initial;
    final.vaddr = 0x08123456u;
    SR_HOST(GE_CAPTURE_VRAM_BASE)[17] = 0x6d;
    SR_HOST(0x00123000u)[9] = 0xa1;
    SR_HOST(0x08145000u)[11] = 0xb2;

    /* The start address is seeded by begin(), including for a runtime capture
     * that begins on a resumed stall PC rather than a fresh list enqueue. */
    assert(ge_capture_begin(argv[1], 77, 0x00123008u, &initial, zbuf));
    assert(ge_capture_add_list(0x00124008u));
    ge_capture_note_memory(0x00123009u, 4);
    ge_capture_note_memory(0x0814500bu, 1);
    ge_capture_note_memory(0x08145fffu, 2);  /* crosses into a third sparse page */
    ge_capture_note_memory(GE_CAPTURE_VRAM_BASE + 17u, 1);  /* covered eagerly */
    SR_HOST(0x00123000u)[9] = 0xff;
    SR_HOST(GE_CAPTURE_VRAM_BASE)[17] = 0xee;
    assert(ge_capture_end(&final));

    GeCaptureFixture fixture;
    assert(ge_capture_load(argv[1], &fixture));
    assert(fixture.frame == 77 && fixture.list_count == 2);
    assert(fixture.list_addrs[0] == 0x00123008u && fixture.list_addrs[1] == 0x00124008u);
    assert(fixture.page_count == 3);
    memset(SR_HOST(GE_CAPTURE_VRAM_BASE), 0, GE_CAPTURE_VRAM_SIZE);
    memset(SR_HOST(0x00123000u), 0, GE_CAPTURE_PAGE_SIZE);
    memset(SR_HOST(0x08145000u), 0, GE_CAPTURE_PAGE_SIZE * 2u);
    memset(zbuf, 0, sizeof(zbuf));
    GeState restored;
    assert(ge_capture_apply(&fixture, &restored, zbuf));
    assert(!memcmp(&restored, &initial, sizeof(initial)));
    assert(SR_HOST(GE_CAPTURE_VRAM_BASE)[17] == 0x6d);
    assert(SR_HOST(0x00123000u)[9] == 0xa1);
    assert(SR_HOST(0x08145000u)[11] == 0xb2);
    assert(zbuf[12345] == (uint16_t)(12345u ^ 0x5a5au));
    ge_capture_free(&fixture);
    free(arena);
    remove(argv[1]);
    puts("ge capture selftest: OK");
    return 0;
}
