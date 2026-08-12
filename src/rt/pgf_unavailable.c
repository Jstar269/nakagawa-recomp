// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

/* Fail-closed backend for source distributions that exclude the lineage-sensitive
 * PGF parser/rasterizer. It deliberately does not fabricate fonts or glyph success. */

#include "pgf_api.h"
#ifdef _WIN32
#include <wchar.h>
#endif

struct PGF { unsigned unused; };

PGF *pgf_open(const char *path) { (void)path; return NULL; }
#ifdef _WIN32
PGF *pgf_open_w(const wchar_t *path) { (void)path; return NULL; }
#endif
PGF *pgf_open_memory(const void *data, size_t size) { (void)data; (void)size; return NULL; }
void pgf_close(PGF *p) { (void)p; }
int pgf_has_char(const PGF *p, int char_code) { (void)p; (void)char_code; return 0; }
void pgf_get_font_info(const PGF *p, uint32_t guest_info) { (void)p; (void)guest_info; }
int pgf_get_char_info(const PGF *p, int char_code, int alt_char_code, uint32_t guest_info) {
    (void)p; (void)char_code; (void)alt_char_code; (void)guest_info; return 0;
}
int pgf_draw_glyph(const PGF *p, int char_code, int alt_char_code, uint32_t guest_image) {
    (void)p; (void)char_code; (void)alt_char_code; (void)guest_image; return 0;
}
int pgf_draw_glyph_by_id(const PGF *p, int glyph_id, uint32_t guest_image) {
    (void)p; (void)glyph_id; (void)guest_image; return 0;
}
