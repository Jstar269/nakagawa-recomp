// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Host-side definitions for builds that link the project-authored PGF reader
 * (src/rt/pgf_public.c) without the runtime: the player and the native core tests.
 *
 * The reader's guest-memory paths (font info, character info, drawing) reference two
 * runtime globals. Those paths are never reached by font validation, and g_mem is NULL
 * here, so any guest access fails closed inside the reader. */

#include <stddef.h>
#include <stdint.h>

uint8_t *g_mem = NULL;

void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes) {
    (void)addr;
    (void)bytes;
}
