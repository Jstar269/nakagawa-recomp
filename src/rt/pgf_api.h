// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#ifndef SR_PGF_API_H
#define SR_PGF_API_H

#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#include <wchar.h>
#endif

typedef struct PGF PGF;

PGF *pgf_open(const char *path);
#ifdef _WIN32
PGF *pgf_open_w(const wchar_t *path);
#endif
PGF *pgf_open_memory(const void *data, size_t size);
void pgf_close(PGF *p);
int pgf_has_char(const PGF *p, int char_code);
void pgf_get_font_info(const PGF *p, uint32_t guest_info);
int pgf_get_char_info(const PGF *p, int char_code, int alt_char_code, uint32_t guest_info);
int pgf_draw_glyph(const PGF *p, int char_code, int alt_char_code, uint32_t guest_image);
int pgf_draw_glyph_by_id(const PGF *p, int glyph_id, uint32_t guest_image);

#endif
