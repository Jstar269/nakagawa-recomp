// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* The console's own system font list, as its font library reports it (oracle case font-list,
 * evidence id PSP-FONTLIST-001).
 *
 * A PSP title's font library reads the system font registry itself and hands the per-font
 * values to the title through the font list. This case asks the resident font library
 * (sceLibFont) for exactly that list and records what it returns: the font count, the raw
 * bytes of every list entry, and the raw bytes of every per-index info record. The probe makes
 * no registry call of its own and opens no file: the only registry reads are the library's,
 * made the way every font-using title makes them. Names inside the entries are console strings
 * and stay in the private host0 log; the parser never decides what a console should hold.
 *
 * Shared by the probe (fixtures/psp_oracle/probe.c, CASE=font-list, linking
 * libfont_imports.S) and a host harness (fixtures/psp_oracle/io_cases_host.c, mode fontlist)
 * that runs the same code against a fake font library.
 *
 * Safety contract (checked by the host harness tests in tools/test_psp_oracle.py and by the
 * build-time import gate, which refuses any sceReg import in this case):
 *  - Exactly five library calls, in this order: sceFontNewLib, sceFontGetNumFontList,
 *    sceFontGetFontList, sceFontGetFontInfoByIndexNumber (once per index), sceFontDoneLib.
 *    No font is opened, no file is read, nothing is written anywhere but the record stream.
 *  - The library allocates only through the callbacks below, into one static arena; a request
 *    the arena cannot hold is refused (NULL), never grown. free is a counted no-op: the arena
 *    is released as a whole when the probe exits.
 *  - Every loop is bounded (FONTLIST_ENTRIES_MAX entries) and every buffer is static.
 *  - Each call is preceded by a step marker, so a hang names the call it is in.
 *
 * Layout facts used here (public PSP font-library ABI; the raw bytes are recorded in full so an
 * offline reading can correct any field interpretation without re-measuring):
 *  - SceFontNewLibParams is eleven 32-bit words: user data, font count, cache address, then the
 *    alloc, free, open, close, read, seek, error and io-finish callbacks. Only alloc and free
 *    are given; the file callbacks stay null because no font is opened.
 *  - SceFontStyle is 168 bytes: five floats (h, v, hres, vres, weight), six u16 codes (family,
 *    style, sub-style, language, region, country), a 64-byte name, a 64-byte file name, then
 *    attributes and expire words. Entries record words 0-7 and 40-41 as out0-out9 plus the whole
 *    168 bytes as raw_hex.
 *
 * The includer provides the five sceLibFont functions declared below (libfont_imports.S on the
 * PSP; the fake library on the host). */

#ifndef NAKAGAWA_FONT_LIST_H
#define NAKAGAWA_FONT_LIST_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define FONTLIST_TEST_ID "PSP-FONTLIST-001"
#define FONTLIST_ENTRIES_MAX 64u
#define FONTLIST_STYLE_BYTES 168u
#define FONTLIST_ENTRY_STRIDE 256u
#define FONTLIST_ARENA_BYTES (512u * 1024u)
#define FONTLIST_ARENA_ALIGN 16u
#define FONTLIST_LINE_MAX 1024u
#define FONTLIST_UNTOUCHED 0xa5a5a5a5u
#define FONTLIST_WORDS_RECORDED 10u

_Static_assert(FONTLIST_STYLE_BYTES <= FONTLIST_ENTRY_STRIDE, "an entry must fit its stride");
_Static_assert(FONTLIST_STYLE_BYTES * 2u + 256u < FONTLIST_LINE_MAX,
               "a record with raw_hex must fit FONTLIST_LINE_MAX");

/* The public SceFontNewLibParams layout: eleven words. */
typedef struct FontListNewLibParams {
    void *userDataAddr;
    uint32_t numFonts;
    void *cacheDataAddr;
    void *(*allocFuncAddr)(void *data, uint32_t size);
    void (*freeFuncAddr)(void *data, void *p);
    void *openFuncAddr;
    void *closeFuncAddr;
    void *readFuncAddr;
    void *seekFuncAddr;
    void *errorFuncAddr;
    void *ioFinishFuncAddr;
} FontListNewLibParams;

/* Eleven pointer-sized slots on the PSP (44 bytes) and on a 64-bit host (numFonts pads to a
 * slot), so the callback offsets the library reads are the same on both. */
_Static_assert(sizeof(FontListNewLibParams) == 11u * sizeof(void *),
               "FontListNewLibParams is eleven slots");

/* Provided by the includer. Library handles travel as 32-bit words. */
uint32_t sceFontNewLib(FontListNewLibParams *params, uint32_t *errorCode);
int sceFontDoneLib(uint32_t libHandle);
int sceFontGetNumFontList(uint32_t libHandle, uint32_t *errorCode);
int sceFontGetFontList(uint32_t libHandle, void *fontStyles, int numFonts);
int sceFontGetFontInfoByIndexNumber(uint32_t libHandle, void *fontStyle, int fontIndex);

typedef struct FontList {
    void (*emit)(void *ctx, const char *line, size_t length);
    void (*step)(void *ctx, const char *step);
    void *ctx;
    uint32_t records;
    uint32_t entries;
    uint32_t infos;
    uint32_t failures;
    uint32_t alloc_calls;
    uint32_t alloc_refused;
    uint32_t free_calls;
    uint32_t arena_used; /* high-water bytes */
} FontList;

static uint8_t s_fontlist_arena[FONTLIST_ARENA_BYTES] __attribute__((aligned(16)));
static uint32_t s_fontlist_arena_top;
static uint8_t s_fontlist_styles[FONTLIST_ENTRIES_MAX * FONTLIST_ENTRY_STRIDE];
static uint8_t s_fontlist_info[FONTLIST_ENTRY_STRIDE];
static char s_fontlist_line[FONTLIST_LINE_MAX];

static void *fontlist_alloc(void *data, uint32_t size) {
    FontList *f = (FontList *)data;
    f->alloc_calls++;
    uint32_t want = size ? size : FONTLIST_ARENA_ALIGN;
    uint32_t aligned = (want + FONTLIST_ARENA_ALIGN - 1u) & ~(FONTLIST_ARENA_ALIGN - 1u);
    if (aligned < want || aligned > FONTLIST_ARENA_BYTES - s_fontlist_arena_top) {
        f->alloc_refused++;
        return NULL;
    }
    void *block = s_fontlist_arena + s_fontlist_arena_top;
    s_fontlist_arena_top += aligned;
    if (s_fontlist_arena_top > f->arena_used) f->arena_used = s_fontlist_arena_top;
    return block;
}

static void fontlist_free(void *data, void *block) {
    (void)block; /* the arena is released as a whole; nothing is reused */
    ((FontList *)data)->free_calls++;
}

static uint32_t fontlist_word(const uint8_t *bytes, size_t offset) {
    return (uint32_t)bytes[offset] | ((uint32_t)bytes[offset + 1u] << 8) |
           ((uint32_t)bytes[offset + 2u] << 16) | ((uint32_t)bytes[offset + 3u] << 24);
}

/* out0-out7 = words 0-7 of a style (floats as bit patterns, then the packed u16 codes), out8 =
 * word 40 (attributes), out9 = word 41 (expire). */
static void fontlist_style_words(const uint8_t *style, uint32_t *out) {
    for (size_t i = 0; i < 8u; i++) out[i] = fontlist_word(style, i * 4u);
    out[8] = fontlist_word(style, 0xA0u);
    out[9] = fontlist_word(style, 0xA4u);
}

static void fontlist_record(FontList *f, const char *case_id, int pass, uint32_t result,
                            const uint32_t *out, size_t out_count,
                            const uint8_t *raw, size_t raw_length) {
    static const char digits[] = "0123456789abcdef";
    int used = snprintf(s_fontlist_line, FONTLIST_LINE_MAX,
                        "NAKAGAWA_PSP_TEST schema=1 test_id=" FONTLIST_TEST_ID
                        " case_id=%s status=%s result=0x%08x",
                        case_id, pass ? "PASS" : "FAIL", (unsigned int)result);
    if (used <= 0 || (size_t)used >= FONTLIST_LINE_MAX) return;
    for (size_t i = 0; i < out_count; i++) {
        int more = snprintf(s_fontlist_line + used, FONTLIST_LINE_MAX - (size_t)used,
                            " out%u=0x%08x", (unsigned int)i, (unsigned int)out[i]);
        if (more <= 0 || (size_t)(used + more) >= FONTLIST_LINE_MAX) return;
        used += more;
    }
    if (raw && raw_length) {
        int more = snprintf(s_fontlist_line + used, FONTLIST_LINE_MAX - (size_t)used, " raw_hex=");
        if (more <= 0 || (size_t)(used + more) + raw_length * 2u + 1u >= FONTLIST_LINE_MAX) return;
        used += more;
        for (size_t i = 0; i < raw_length; i++) {
            s_fontlist_line[used++] = digits[raw[i] >> 4];
            s_fontlist_line[used++] = digits[raw[i] & 0xFu];
        }
        s_fontlist_line[used] = '\0';
    }
    if ((size_t)used + 1u >= FONTLIST_LINE_MAX) return;
    s_fontlist_line[used++] = '\n';
    s_fontlist_line[used] = '\0';
    f->emit(f->ctx, s_fontlist_line, (size_t)used);
    f->records++;
}

static void fontlist_done(FontList *f) {
    uint32_t out[7];
    out[0] = f->entries;
    out[1] = f->infos;
    out[2] = f->arena_used;
    out[3] = f->alloc_calls;
    out[4] = f->free_calls;
    out[5] = f->alloc_refused;
    out[6] = f->failures;
    fontlist_record(f, "font-list-done", f->failures == 0u, 0u, out, 7u, NULL, 0u);
}

/* An error word the library did not write stays FONTLIST_UNTOUCHED; a call is judged by its
 * return value, and the error word is recorded for the reading. */
static int fontlist_error_clean(uint32_t error) {
    return error == 0u || error == FONTLIST_UNTOUCHED;
}

static void fontlist_run(FontList *f) {
    uint32_t out[FONTLIST_WORDS_RECORDED];
    char name[32];
    FontListNewLibParams params;

    s_fontlist_arena_top = 0u;
    memset(s_fontlist_arena, 0, sizeof(s_fontlist_arena));
    memset(&params, 0, sizeof(params));
    params.userDataAddr = f;
    params.numFonts = 1u;
    params.allocFuncAddr = fontlist_alloc;
    params.freeFuncAddr = fontlist_free;

    f->step(f->ctx, "newlib");
    uint32_t error = FONTLIST_UNTOUCHED;
    const uint32_t lib = sceFontNewLib(&params, &error);
    const int lib_ok = lib != 0u && fontlist_error_clean(error);
    out[0] = error;
    out[1] = params.numFonts;
    out[2] = f->arena_used;
    out[3] = f->alloc_calls;
    out[4] = f->alloc_refused;
    fontlist_record(f, "font-list-newlib", lib_ok, lib, out, 5u, NULL, 0u);
    if (lib == 0u) {
        f->failures++;
        fontlist_done(f);
        return;
    }
    if (!lib_ok) f->failures++;

    f->step(f->ctx, "count");
    error = FONTLIST_UNTOUCHED;
    const int count = sceFontGetNumFontList(lib, &error);
    const int count_in_range = count >= 0 && (uint32_t)count <= FONTLIST_ENTRIES_MAX;
    uint32_t requested = 0u;
    if (count > 0) {
        requested = (uint32_t)count;
        if (requested > FONTLIST_ENTRIES_MAX) requested = FONTLIST_ENTRIES_MAX;
    }
    const int count_ok = count_in_range && fontlist_error_clean(error);
    out[0] = error;
    out[1] = requested;
    fontlist_record(f, "font-list-count", count_ok, (uint32_t)count, out, 2u, NULL, 0u);
    if (!count_ok) f->failures++;

    if (requested > 0u) {
        memset(s_fontlist_styles, 0xA5, sizeof(s_fontlist_styles));
        f->step(f->ctx, "list");
        const int list_rc = sceFontGetFontList(lib, s_fontlist_styles, (int)requested);
        const int list_ok = list_rc >= 0;
        out[0] = requested;
        fontlist_record(f, "font-list-list", list_ok, (uint32_t)list_rc, out, 1u, NULL, 0u);
        if (!list_ok) f->failures++;
        for (uint32_t i = 0; i < requested; i++) {
            const uint8_t *entry = s_fontlist_styles + (size_t)i * FONTLIST_STYLE_BYTES;
            snprintf(name, sizeof(name), "font-list-entry-%04u", (unsigned int)i);
            fontlist_style_words(entry, out);
            fontlist_record(f, name, list_ok, 0u, out, FONTLIST_WORDS_RECORDED,
                            entry, FONTLIST_STYLE_BYTES);
            f->entries++;
        }
        for (uint32_t i = 0; i < requested; i++) {
            snprintf(name, sizeof(name), "info:%04u", (unsigned int)i);
            f->step(f->ctx, name);
            memset(s_fontlist_info, 0xA5, sizeof(s_fontlist_info));
            const int info_rc = sceFontGetFontInfoByIndexNumber(lib, s_fontlist_info, (int)i);
            snprintf(name, sizeof(name), "font-info-%04u", (unsigned int)i);
            fontlist_style_words(s_fontlist_info, out);
            fontlist_record(f, name, info_rc == 0, (uint32_t)info_rc, out, FONTLIST_WORDS_RECORDED,
                            s_fontlist_info, FONTLIST_STYLE_BYTES);
            if (info_rc == 0) f->infos++; else f->failures++;
        }
    }

    f->step(f->ctx, "donelib");
    const int done_rc = sceFontDoneLib(lib);
    out[0] = f->free_calls;
    out[1] = f->arena_used;
    fontlist_record(f, "font-list-donelib", done_rc == 0, (uint32_t)done_rc, out, 2u, NULL, 0u);
    if (done_rc != 0) f->failures++;
    fontlist_done(f);
}

#endif /* NAKAGAWA_FONT_LIST_H */
