// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later

/* * char-glyph path ACX needs: parse the font, report metrics, and rasterise glyph bitmaps into the
 * guest buffer the game uploads as a GE texture. See pgf.h. */

#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "recomp.h"
#include "pgf.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <wchar.h>

/* Glyph metric flags (PPSSPP PGF.h). */
enum {
    FONT_PGF_BMP_H_ROWS = 0x01,
    FONT_PGF_BMP_V_ROWS = 0x02,
    FONT_PGF_BMP_OVERLAY = 0x03,
    FONT_PGF_METRIC_DIMENSION_INDEX = 0x04,
    FONT_PGF_METRIC_BEARING_X_INDEX = 0x08,
    FONT_PGF_METRIC_BEARING_Y_INDEX = 0x10,
    FONT_PGF_METRIC_ADVANCE_INDEX = 0x20,
    FONT_PGF_CHARGLYPH = 0x20,
};

#pragma pack(push, 1)
typedef struct {
    uint16_t headerOffset, headerSize;
    char     PGFMagic[4];
    int32_t  revision, version;
    int32_t  charMapLength, charPointerLength, charMapBpe, charPointerBpe;
    uint8_t  pad1[2]; uint8_t bpp; uint8_t pad2[1];
    int32_t  hSize, vSize, hResolution, vResolution;
    uint8_t  pad3[1];
    char     fontName[64]; char fontType[64];
    uint8_t  pad4[1];
    uint16_t firstGlyph, lastGlyph;
    uint8_t  pad5[26];
    int32_t  maxAscender, maxDescender, maxLeftXAdjust, maxBaseYAdjust, minCenterXAdjust, maxTopYAdjust;
    int32_t  maxAdvance[2], maxSize[2];
    uint16_t maxGlyphWidth, maxGlyphHeight;
    uint8_t  pad6[2];
    uint8_t  dimTableLength, xAdjustTableLength, yAdjustTableLength, advanceTableLength;
    uint8_t  pad7[102];
    int32_t  shadowMapLength, shadowMapBpe;
    float    unknown1;
    int32_t  shadowScale[2];
    uint8_t  pad8[8];
} PGFHeader;
#pragma pack(pop)

typedef struct {
    int w, h, left, top, flags, shadowFlags, shadowID, advanceH, advanceV;
    int dimW, dimH, xAdjH, xAdjV, yAdjH, yAdjV;
    uint32_t ptr;   /* byte offset into fontData */
} PGFGlyph;

struct PGF {
    uint8_t *file;
    size_t   fileSize;
    PGFHeader header;
    int *dim0, *dim1, dimLen;
    int *xa0, *xa1, xaLen;
    int *ya0, *ya1, yaLen;
    int *adv0, *adv1, advLen;
    int *charmap, charMapLen;
    int  firstGlyph;
    PGFGlyph *glyphs;
    int  nGlyphs;
    const uint8_t *fontData;
    size_t fontDataSize;
};

/* LSB-first bit reader over a byte stream (PPSSPP's u32-word getBits is bit-identical to this on
 * little-endian input). */
static int pgf_getBits(int numBits, const uint8_t *buf, size_t pos) {
    int v = 0;
    for (int i = 0; i < numBits; i++) {
        size_t bit = pos + (size_t)i;
        int b = (buf[bit >> 3] >> (bit & 7)) & 1;
        v |= b << i;
    }
    return v;
}
static int pgf_consume(int numBits, const uint8_t *buf, size_t *pos) {
    int v = pgf_getBits(numBits, buf, *pos);
    *pos += (size_t)numBits;
    return v;
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void read_char_glyph(PGF *p, size_t charPtr, PGFGlyph *g) {
    const uint8_t *fd = p->fontData;
    charPtr += 14;                          /* skip size field */
    g->w = pgf_consume(7, fd, &charPtr);
    g->h = pgf_consume(7, fd, &charPtr);
    g->left = pgf_consume(7, fd, &charPtr); if (g->left >= 64) g->left -= 128;
    g->top  = pgf_consume(7, fd, &charPtr); if (g->top  >= 64) g->top  -= 128;
    g->flags = pgf_consume(6, fd, &charPtr);
    g->shadowFlags  = pgf_consume(2, fd, &charPtr) << (2 + 3);
    g->shadowFlags |= pgf_consume(2, fd, &charPtr) << 3;
    g->shadowFlags |= pgf_consume(3, fd, &charPtr);
    g->shadowID = pgf_consume(9, fd, &charPtr);

    if ((g->flags & FONT_PGF_METRIC_DIMENSION_INDEX) == FONT_PGF_METRIC_DIMENSION_INDEX) {
        int i = pgf_consume(8, fd, &charPtr);
        if (i < p->dimLen) { g->dimW = p->dim0[i]; g->dimH = p->dim1[i]; }
    } else { g->dimW = pgf_consume(32, fd, &charPtr); g->dimH = pgf_consume(32, fd, &charPtr); }

    if ((g->flags & FONT_PGF_METRIC_BEARING_X_INDEX) == FONT_PGF_METRIC_BEARING_X_INDEX) {
        int i = pgf_consume(8, fd, &charPtr);
        if (i < p->xaLen) { g->xAdjH = p->xa0[i]; g->xAdjV = p->xa1[i]; }
    } else { g->xAdjH = pgf_consume(32, fd, &charPtr); g->xAdjV = pgf_consume(32, fd, &charPtr); }

    if ((g->flags & FONT_PGF_METRIC_BEARING_Y_INDEX) == FONT_PGF_METRIC_BEARING_Y_INDEX) {
        int i = pgf_consume(8, fd, &charPtr);
        if (i < p->yaLen) { g->yAdjH = p->ya0[i]; g->yAdjV = p->ya1[i]; }
    } else { g->yAdjH = pgf_consume(32, fd, &charPtr); g->yAdjV = pgf_consume(32, fd, &charPtr); }

    if ((g->flags & FONT_PGF_METRIC_ADVANCE_INDEX) == FONT_PGF_METRIC_ADVANCE_INDEX) {
        int i = pgf_consume(8, fd, &charPtr);
        if (i < p->advLen) { g->advanceH = p->adv0[i]; g->advanceV = p->adv1[i]; }
    } else { g->advanceH = pgf_consume(32, fd, &charPtr); g->advanceV = pgf_consume(32, fd, &charPtr); }

    g->ptr = (uint32_t)(charPtr / 8);
}

static PGF *pgf_parse_owned(uint8_t *owned, size_t size) {
    if (!owned || size < sizeof(PGFHeader)) { free(owned); return NULL; }
    PGF *p = (PGF *)calloc(1, sizeof(PGF));
    if (!p) { free(owned); return NULL; }
    p->fileSize = size;
    p->file = owned;

    memcpy(&p->header, p->file, sizeof(PGFHeader));
    if (memcmp(p->header.PGFMagic, "PGF0", 4) != 0) { free(p->file); free(p); return NULL; }
    if (p->header.charMapLength < 0 || p->header.charPointerLength <= 0 ||
        p->header.charMapLength > 0x100000 || p->header.charPointerLength > 0x100000 ||
        p->header.charMapBpe <= 0 || p->header.charMapBpe > 32 ||
        p->header.charPointerBpe <= 0 || p->header.charPointerBpe > 32) goto fail;

    const uint8_t *ptr = p->file + sizeof(PGFHeader);
    const uint8_t *end = p->file + p->fileSize;
    int compLen1 = 0, compLen2 = 0;
    if (p->header.revision == 3) {
        if ((size_t)(end - ptr) < 20) goto fail;
        compLen1 = (int)(rd32(ptr + 4) & 0xFFFF);
        compLen2 = (int)(rd32(ptr + 12) & 0xFFFF);
        ptr += 20;   /* PGFHeaderRev3Extra */
    }

    /* dimension / xAdjust / yAdjust / advance tables: len pairs of u32. */
    #define RDTBL(a0, a1, len) do { \
        int n = (len); if ((size_t)(end - ptr) < (size_t)n * 8) goto fail; \
        a0 = (int*)calloc((size_t)(n > 0 ? n : 1), sizeof(int)); a1 = (int*)calloc((size_t)(n > 0 ? n : 1), sizeof(int)); \
        if (!(a0) || !(a1)) goto fail; \
        for (int i = 0; i < n; i++) { a0[i] = (int)rd32(ptr); ptr += 4; a1[i] = (int)rd32(ptr); ptr += 4; } } while (0)
    p->dimLen = p->header.dimTableLength;          RDTBL(p->dim0, p->dim1, p->dimLen);
    p->xaLen  = p->header.xAdjustTableLength;       RDTBL(p->xa0, p->xa1, p->xaLen);
    p->yaLen  = p->header.yAdjustTableLength;       RDTBL(p->ya0, p->ya1, p->yaLen);
    p->advLen = p->header.advanceTableLength;       RDTBL(p->adv0, p->adv1, p->advLen);
    #undef RDTBL

    if (p->header.shadowMapLength < 0 || p->header.shadowMapBpe < 0 || p->header.shadowMapBpe > 32) goto fail;
    int shadowCharMapSize = (int)((((size_t)p->header.shadowMapLength * (size_t)p->header.shadowMapBpe + 31) & ~(size_t)31) / 8);
    if ((size_t)(end - ptr) < (size_t)shadowCharMapSize) goto fail;
    ptr += shadowCharMapSize;                       /* shadow charmap (unused here) */

    if (p->header.revision == 3) {
        size_t compBytes = (size_t)compLen1 * 4 + (size_t)compLen2 * 4;
        if (compLen1 < 0 || compLen2 < 0 || (size_t)(end - ptr) < compBytes) goto fail;
        ptr += compBytes;
    }

    int charMapSize = (int)((((size_t)p->header.charMapLength * (size_t)p->header.charMapBpe + 31) & ~(size_t)31) / 8);
    if ((size_t)(end - ptr) < (size_t)charMapSize) goto fail;
    const uint8_t *charMap = ptr; ptr += charMapSize;
    int charPtrSize = (int)((((size_t)p->header.charPointerLength * (size_t)p->header.charPointerBpe + 31) & ~(size_t)31) / 8);
    if ((size_t)(end - ptr) < (size_t)charPtrSize) goto fail;
    const uint8_t *charPtrTable = ptr; ptr += charPtrSize;

    if (ptr < p->file || ptr >= p->file + p->fileSize) goto fail;
    p->fontData = ptr;
    p->fontDataSize = (size_t)(p->file + p->fileSize - ptr);

    p->charMapLen = p->header.charMapLength;
    p->charmap = (int *)calloc((size_t)(p->charMapLen > 0 ? p->charMapLen : 1), sizeof(int));
    if (!p->charmap) goto fail;
    for (int i = 0; i < p->charMapLen; i++) {
        int c = pgf_getBits(p->header.charMapBpe, charMap, (size_t)i * p->header.charMapBpe);
        if (c >= p->header.charPointerLength) c = 65535;
        p->charmap[i] = c;
    }

    p->nGlyphs = p->header.charPointerLength;
    p->glyphs = (PGFGlyph *)calloc((size_t)(p->nGlyphs > 0 ? p->nGlyphs : 1), sizeof(PGFGlyph));
    if (!p->glyphs) goto fail;
    p->firstGlyph = p->header.firstGlyph;
    for (int i = 0; i < p->nGlyphs; i++) {
        int cp = pgf_getBits(p->header.charPointerBpe, charPtrTable, (size_t)i * p->header.charPointerBpe);
        if (cp < 0 || (size_t)cp * 4 >= p->fontDataSize) goto fail;
        read_char_glyph(p, (size_t)cp * 4 * 8, &p->glyphs[i]);
    }
    return p;
fail:
    pgf_close(p);
    return NULL;
}

PGF *pgf_open_memory(const void *data, size_t size) {
    if (!data || size < sizeof(PGFHeader) || size > 16u * 1024u * 1024u) return NULL;
    uint8_t *owned = (uint8_t *)calloc(1, size + 64);  /* bit-reader safety padding */
    if (!owned) return NULL;
    memcpy(owned, data, size);
    return pgf_parse_owned(owned, size);
}

static PGF *pgf_open_file(FILE *f) {
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < (long)sizeof(PGFHeader) || sz > 16L * 1024L * 1024L) { fclose(f); return NULL; }
    uint8_t *owned = (uint8_t *)calloc(1, (size_t)sz + 64);
    if (!owned || fread(owned, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(owned); return NULL;
    }
    fclose(f);
    return pgf_parse_owned(owned, (size_t)sz);
}

PGF *pgf_open(const char *path) {
    return pgf_open_file(path ? fopen(path, "rb") : NULL);
}

#ifdef _WIN32
PGF *pgf_open_w(const wchar_t *path) {
    return pgf_open_file(path ? _wfopen(path, L"rb") : NULL);
}
#endif

void pgf_close(PGF *p) {
    if (!p) return;
    free(p->dim0); free(p->dim1); free(p->xa0); free(p->xa1);
    free(p->ya0); free(p->ya1); free(p->adv0); free(p->adv1);
    free(p->charmap); free(p->glyphs); free(p->file); free(p);
}

static int get_char_glyph(const PGF *p, int charCode, PGFGlyph *out) {
    if (charCode < p->firstGlyph) return 0;
    charCode -= p->firstGlyph;
    if (charCode < p->charMapLen) charCode = p->charmap[charCode];
    if (charCode < 0 || charCode >= p->nGlyphs) return 0;
    *out = p->glyphs[charCode];
    return 1;
}

int pgf_has_char(const PGF *p, int charCode) {
    PGFGlyph g;
    if (!p) return 0;
    if (!get_char_glyph(p, charCode, &g)) return 0;
    return g.w > 0 && g.h > 0;
}

void pgf_get_font_info(const PGF *p, uint32_t fi) {
    if (!p || !fi) return;
    const PGFHeader *h = &p->header;
    MEM_W32(fi + 0,  (uint32_t)h->maxSize[0]);
    MEM_W32(fi + 4,  (uint32_t)h->maxSize[1]);
    MEM_W32(fi + 8,  (uint32_t)h->maxAscender);
    MEM_W32(fi + 12, (uint32_t)h->maxDescender);
    MEM_W32(fi + 16, (uint32_t)h->maxLeftXAdjust);
    MEM_W32(fi + 20, (uint32_t)h->maxBaseYAdjust);
    MEM_W32(fi + 24, (uint32_t)h->minCenterXAdjust);
    MEM_W32(fi + 28, (uint32_t)h->maxTopYAdjust);
    MEM_W32(fi + 32, (uint32_t)h->maxAdvance[0]);
    MEM_W32(fi + 36, (uint32_t)h->maxAdvance[1]);
    /* float replicas at +40..+76 */
    float ff[10]; ff[0]=(float)h->maxSize[0]/64.f; ff[1]=(float)h->maxSize[1]/64.f;
    ff[2]=(float)h->maxAscender/64.f; ff[3]=(float)h->maxDescender/64.f;
    ff[4]=(float)h->maxLeftXAdjust/64.f; ff[5]=(float)h->maxBaseYAdjust/64.f;
    ff[6]=(float)h->minCenterXAdjust/64.f; ff[7]=(float)h->maxTopYAdjust/64.f;
    ff[8]=(float)h->maxAdvance[0]/64.f; ff[9]=(float)h->maxAdvance[1]/64.f;
    for (int i = 0; i < 10; i++) { uint32_t w; memcpy(&w, &ff[i], 4); MEM_W32(fi + 40 + (uint32_t)i * 4, w); }
    MEM_W16(fi + 80, (uint16_t)h->maxGlyphWidth);
    MEM_W16(fi + 82, (uint16_t)h->maxGlyphHeight);
    MEM_W32(fi + 84, (uint32_t)h->charPointerLength);  /* numGlyphs */
    MEM_W32(fi + 88, (uint32_t)h->shadowMapLength);
    /* PGFFontStyle begins at +0x5c. */
    float stylef[5] = { (float)h->hSize / 64.f, (float)h->vSize / 64.f,
                        (float)h->hResolution / 64.f, (float)h->vResolution / 64.f, 0.f };
    for (int i = 0; i < 5; i++) { uint32_t w; memcpy(&w, &stylef[i], 4); MEM_W32(fi + 92 + (uint32_t)i * 4, w); }
    MEM_W16(fi + 112, 1);  /* sans serif */
    MEM_W16(fi + 114, 1);  /* regular */
    MEM_W16(fi + 116, 0);  /* style sub */
    int japanese = pgf_has_char(p, 0x3042);
    MEM_W16(fi + 118, (uint16_t)(japanese ? 1 : 2));
    MEM_W16(fi + 120, 0); MEM_W16(fi + 122, 1);
    for (int i = 0; i < 64 && h->fontName[i]; i++) MEM_W8(fi + 124 + (uint32_t)i, (uint8_t)h->fontName[i]);
    const char *fileName = japanese ? "jpn0.pgf" : "ltn0.pgf";
    for (int i = 0; fileName[i]; i++) MEM_W8(fi + 188 + (uint32_t)i, (uint8_t)fileName[i]);
    MEM_W32(fi + 252, 0); MEM_W32(fi + 256, 0);
    MEM_W8(fi + 260, h->bpp);
}

int pgf_get_char_info(const PGF *p, int charCode, int altCharCode, uint32_t ci) {
    if (!ci) return 0;
    for (int i = 0; i < 0x3c; i++) MEM_W8(ci + (uint32_t)i, 0);
    PGFGlyph g;
    if (!p || !get_char_glyph(p, charCode, &g)) {
        if (!p || charCode < p->firstGlyph) return 0;
        if (!get_char_glyph(p, altCharCode, &g)) return 0;
    }
    MEM_W32(ci + 0,  (uint32_t)g.w);
    MEM_W32(ci + 4,  (uint32_t)g.h);
    MEM_W32(ci + 8,  (uint32_t)g.left);
    MEM_W32(ci + 12, (uint32_t)g.top);
    MEM_W32(ci + 16, (uint32_t)g.dimW);
    MEM_W32(ci + 20, (uint32_t)g.dimH);
    MEM_W32(ci + 24, (uint32_t)g.yAdjH);                          /* ascender */
    MEM_W32(ci + 28, (uint32_t)(g.yAdjH - g.dimH));               /* descender */
    MEM_W32(ci + 32, (uint32_t)g.xAdjH);
    MEM_W32(ci + 36, (uint32_t)g.yAdjH);
    MEM_W32(ci + 40, (uint32_t)g.xAdjV);
    MEM_W32(ci + 44, (uint32_t)g.yAdjV);
    MEM_W32(ci + 48, (uint32_t)g.advanceH);
    MEM_W32(ci + 52, (uint32_t)g.advanceV);
    MEM_W16(ci + 56, (uint16_t)g.shadowFlags);
    MEM_W16(ci + 58, (uint16_t)g.shadowID);
    return 1;
}

static void set_font_pixel(uint32_t base, int bpl, int bufW, int bufH, int x, int y, uint8_t pix, int fmt) {
    if (x < 0 || x >= bufW || y < 0 || y >= bufH) return;
    static const int pxBytes[5] = { 0, 0, 1, 3, 4 };
    if (fmt < 0 || fmt > 4) return;
    int pb = pxBytes[fmt];
    int bufMaxW = (pb == 0) ? bpl * 2 : bpl / pb;
    if (x >= bufMaxW) return;
    uint32_t a = base + (uint32_t)(y * bpl) + (uint32_t)(pb == 0 ? x / 2 : x * pb);
    switch (fmt) {
        case 0: case 1: {
            uint8_t p4 = pix >> 4;
            uint8_t old = MEM_R8(a);
            if ((x & 1) != fmt) MEM_W8(a, (uint8_t)((p4 << 4) | (old & 0x0F)));
            else                MEM_W8(a, (uint8_t)((old & 0xF0) | p4));
            break;
        }
        case 2: MEM_W8(a, pix); break;
        case 3: MEM_W8(a, pix); MEM_W8(a + 1, pix); MEM_W8(a + 2, pix); break;
        case 4: { uint32_t v = pix; v |= v << 8; v |= v << 16; MEM_W32(a, v); break; }
    }
}

static int draw_glyph(const PGF *p, const PGFGlyph *glyph, uint32_t gi) {
    if (!p || !glyph || !gi) return 0;
    PGFGlyph g = *glyph;

    if (g.w <= 0 || g.h <= 0) return 0;
    int dir = g.flags & FONT_PGF_BMP_OVERLAY;
    if (dir != FONT_PGF_BMP_H_ROWS && dir != FONT_PGF_BMP_V_ROWS) return 0;

    int fmt   = (int)MEM_R32(gi + 0);
    int xPos  = (int)MEM_R32(gi + 4);
    int yPos  = (int)MEM_R32(gi + 8);
    int bufW  = (int)MEM_R16(gi + 12);
    int bufH  = (int)MEM_R16(gi + 14);
    int bpl   = (int)MEM_R16(gi + 16);
    uint32_t base = MEM_R32(gi + 20);
    int x = xPos >> 6, y = yPos >> 6;
    int xFrac = xPos & 0x3F, yFrac = yPos & 0x3F;

    int n = g.w * g.h;
    if (n <= 0 || n > 256 * 256) return 0;
    uint8_t *px = (uint8_t *)calloc((size_t)n, 1);
    if (!px) return 0;
    size_t bitPtr = (size_t)g.ptr * 8;
    int idx = 0;
    while (idx < n && bitPtr + 8 < p->fontDataSize * 8) {
        int nib = pgf_consume(4, p->fontData, &bitPtr);
        int count, value = 0;
        if (nib < 8) { value = pgf_consume(4, p->fontData, &bitPtr); count = nib + 1; }
        else         { count = 16 - nib; }
        for (int i = 0; i < count && idx < n; i++) {
            if (nib >= 8) value = pgf_consume(4, p->fontData, &bitPtr);
            px[idx++] = (uint8_t)(value | (value << 4));
        }
    }

    #define SAMPLE(xx, yy) ( ((xx) < 0 || (yy) < 0 || (xx) >= g.w || (yy) >= g.h) ? 0 : \
        px[(dir == FONT_PGF_BMP_H_ROWS) ? ((yy) * g.w + (xx)) : ((xx) * g.h + (yy))] )

    if (xFrac == 0 && yFrac == 0) {
        for (int yy = 0; yy < g.h; yy++) for (int xx = 0; xx < g.w; xx++)
            set_font_pixel(base, bpl, bufW, bufH, x + xx, y + yy, SAMPLE(xx, yy), fmt);
    } else {
        int w2 = g.w + (xFrac > 0 ? 1 : 0), h2 = g.h + (yFrac > 0 ? 1 : 0);
        for (int yy = 0; yy < h2; yy++) for (int xx = 0; xx < w2; xx++) {
            uint32_t h1 = (uint32_t)SAMPLE(xx - 1, yy - 1) * xFrac + (uint32_t)SAMPLE(xx, yy - 1) * (64 - xFrac);
            uint32_t hh = (uint32_t)SAMPLE(xx - 1, yy)     * xFrac + (uint32_t)SAMPLE(xx, yy)     * (64 - xFrac);
            uint32_t blended = h1 * yFrac + hh * (64 - yFrac);
            set_font_pixel(base, bpl, bufW, bufH, x + xx, y + yy, (uint8_t)(blended >> 12), fmt);
        }
    }
    #undef SAMPLE
    free(px);

    if (base && bpl > 0) {
        extern void sr_gpu_vram_dirty(uint32_t addr, uint32_t bytes);
        static const int pxBytes[5] = { 0, 0, 1, 3, 4 };
        int pb = (fmt >= 0 && fmt <= 4) ? pxBytes[fmt] : 1;
        int bufMaxW = (pb == 0) ? bpl * 2 : bpl / pb;
        int gw = g.w + (xFrac > 0 ? 1 : 0);
        int gh = g.h + (yFrac > 0 ? 1 : 0);
        for (int yy = 0; yy < gh; yy++) {
            int ry = y + yy;
            if (ry < 0 || ry >= bufH) continue;
            int rx0 = x < 0 ? 0 : x;
            int rx1 = x + gw;
            if (rx1 > bufMaxW) rx1 = bufMaxW;
            if (rx0 >= rx1) continue;
            uint32_t start_off = (uint32_t)(pb == 0 ? rx0 / 2 : rx0 * pb);
            uint32_t end_off = (uint32_t)(pb == 0 ? (rx1 - 1) / 2 + 1 : rx1 * pb);
            uint32_t row_addr = base + (uint32_t)(ry * bpl) + start_off;
            uint32_t row_bytes = end_off - start_off;
            if (row_bytes > 0) sr_gpu_vram_dirty(row_addr, row_bytes);
        }
    }
    return 1;
}

int pgf_draw_glyph(const PGF *p, int charCode, int altCharCode, uint32_t gi) {
    if (!p || !gi) return 0;
    PGFGlyph g;
    if (!get_char_glyph(p, charCode, &g)) {
        if (charCode < p->firstGlyph) return 0;
        if (!get_char_glyph(p, altCharCode, &g)) return 0;
    }
    return draw_glyph(p, &g, gi);
}

int pgf_draw_glyph_by_id(const PGF *p, int glyphId, uint32_t gi) {
    if (!p || glyphId < 0 || glyphId >= p->nGlyphs) return 0;
    return draw_glyph(p, &p->glyphs[glyphId], gi);
}
