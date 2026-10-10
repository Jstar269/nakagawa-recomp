// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#ifndef SR_PGF_API_H
#define SR_PGF_API_H

#include <stddef.h>
#include <stdint.h>
#ifdef _WIN32
#include <wchar.h>
#endif

typedef struct PGF PGF;

/* The first rule the reader refuses an image by. PGF_REFUSE_NONE means accepted. The
 * order of the checks and the names are a contract: tools/nk_core/fonts.py mirrors this
 * reader and its parity test compares these codes, so change both together. */
typedef enum PgfRefusal {
    PGF_REFUSE_NONE = 0,
    PGF_REFUSE_TOO_LARGE,      /* the image is above the 16 MiB ceiling */
    PGF_REFUSE_TRUNCATED,      /* the image is below the 392-byte base header, shorter than its
                                  declared header, or a declared section runs past its end */
    PGF_REFUSE_HEADER_OFFSET,  /* the header offset field is not zero */
    PGF_REFUSE_MAGIC,          /* bytes 4..7 of the header are not "PGF0" */
    PGF_REFUSE_REVISION,       /* revision outside 0..3, or a negative version */
    PGF_REFUSE_HEADER_SIZE,    /* declared header size is neither 392 nor 412, or 412 without revision 3 */
    PGF_REFUSE_GLYPH_RANGE,    /* first glyph index greater than last glyph index */
    PGF_REFUSE_COUNTS,         /* a count or bit width, or the shadow map, is out of range */
    PGF_REFUSE_NO_GLYPHS,      /* the character-pointer count is zero */
    PGF_REFUSE_GLYPH           /* a glyph record, shadow, or composite fails the reader's checks */
} PgfRefusal;

/* What the reader found in an image it accepted, or the refusal it gave for one it did not.
 * Coverage flags say whether the character map resolves a drawable glyph for a probe code:
 * has_latin needs U+0041 and U+0061, has_kana needs U+3042 or U+30A2, has_hangul needs
 * U+AC00 or U+D55C. They feed the slot classification in nk_font.c. */
typedef struct PgfVerdict {
    PgfRefusal refusal;
    uint32_t revision;
    uint32_t header_size;
    uint32_t first_glyph;
    uint32_t last_glyph;
    uint32_t glyph_count;     /* character-pointer entries: the number of glyphs */
    uint32_t char_map_count;  /* character-map entries: the code span from first to last */
    int32_t nominal_h;        /* header 0x24: horizontal size, 26.6 units */
    int32_t nominal_v;        /* header 0x28: vertical size, 26.6 units */
    uint8_t has_latin;
    uint8_t has_kana;
    uint8_t has_hangul;
} PgfVerdict;

/* Run the reader's own open checks on an in-memory image without copying it or keeping
 * it. Returns 1 when the reader would accept the image, and then fills every verdict field.
 * Returns 0 and sets verdict->refusal to the first rule the image fails. Never touches
 * guest memory. */
int pgf_validate_memory(const void *data, size_t size, PgfVerdict *verdict);

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
