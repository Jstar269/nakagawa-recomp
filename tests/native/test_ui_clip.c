/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Regression for the blank library window: a scoped clip around the hero art
 * must leave the renderer exactly as it found it. Runs on SDL's software
 * renderer over a memory surface, so it needs no window or GPU.
 *
 * The UTF-8 truncation checks (run_utf8_checks) are pure string checks on the
 * ui_clip.h helpers; they run before the renderer is created. */

#include "ui_clip.h"
#include "nk_types.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond, msg)                                               \
    do {                                                               \
        if (!(cond)) {                                                 \
            fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__);    \
            g_failures++;                                              \
        }                                                              \
    } while (0)

/* Paint the whole target white and report whether the given pixel took it. */
static bool pixel_is_white_after_full_fill(SDL_Renderer *ren, SDL_Surface *surface, int x, int y) {
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    SDL_SetRenderDrawColor(ren, 255, 255, 255, 255);
    SDL_FRect all = { 0.0f, 0.0f, (float)surface->w, (float)surface->h };
    SDL_RenderFillRect(ren, &all);
    SDL_FlushRenderer(ren);
    Uint8 r = 0, g = 0, b = 0, a = 0;
    if (!SDL_ReadSurfacePixel(surface, x, y, &r, &g, &b, &a)) return false;
    return r == 255 && g == 255 && b == 255;
}

/* ---- UTF-8 truncation: pure string checks on the ui_clip.h helpers ----
 * No SDL calls. They run first, so they execute even if no renderer can be made. */

/* Structure check of s[0..n): no stray continuation byte, no orphan lead byte, and
 * no sequence cut short by the end of the range. */
static bool utf8_is_valid(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < n) {
        size_t extra;
        if (p[i] < 0x80) extra = 0;
        else if (p[i] >= 0xC2 && p[i] <= 0xDF) extra = 1;
        else if (p[i] >= 0xE0 && p[i] <= 0xEF) extra = 2;
        else if (p[i] >= 0xF0 && p[i] <= 0xF4) extra = 3;
        else return false;
        if (extra >= n - i) return false;
        for (size_t j = 1; j <= extra; j++) {
            if ((p[i + j] & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

/* Independent oracle: the longest prefix of s, at most limit bytes, that is valid
 * UTF-8 on its own. For the well-formed inputs here that is the longest prefix
 * ending on a character boundary. */
static size_t oracle_kept(const char *s, size_t limit) {
    size_t len = strlen(s);
    size_t k = limit < len ? limit : len;
    while (k > 0 && !utf8_is_valid(s, k)) k--;
    return k;
}

/* Length of s, reading at most cap bytes, so a missing terminator is not overrun. */
static size_t bounded_len(const char *s, size_t cap) {
    size_t n = 0;
    while (n < cap && s[n] != '\0') n++;
    return n;
}

/* Output contract of ui_clip_ellipsize_utf8: NUL-terminated inside out_size, the
 * kept prefix is the expected one, valid UTF-8, and "..." follows it. */
static bool ellipsized_ok(const char *out, size_t out_size, size_t kept, size_t want) {
    size_t len = bounded_len(out, out_size);
    if (len >= out_size) return false;
    return kept == want && len == kept + 3 && utf8_is_valid(out, len) &&
           memcmp(out + kept, "...", 3) == 0;
}

/* Every byte offset of a string built from one multi-byte character, cut to a
 * 64-byte output (48 bytes of text plus the marker). */
static void check_every_cut(const char *label, const char *unit, size_t reps) {
    char src[256];
    size_t unit_len = strlen(unit);
    size_t n = 2;
    memcpy(src, "ab", 2);
    for (size_t r = 0; r < reps; r++) {
        memcpy(src + n, unit, unit_len);
        n += unit_len;
    }
    src[n] = '\0';
    char out[64];
    for (size_t limit = 0; limit <= n; limit++) {
        size_t kept = ui_clip_ellipsize_utf8(out, sizeof out, src, limit);
        size_t cap = sizeof out - 4;
        size_t want = oracle_kept(src, limit < cap ? limit : cap);
        if (!ellipsized_ok(out, sizeof out, kept, want)) {
            fprintf(stderr, "FAIL: %s text cut at byte %zu: kept %zu, want %zu\n", label, limit, kept, want);
            g_failures++;
        }
    }
}

/* The title path (draw_text_ellipsized) writes into an NK_MAX_TITLE_LEN buffer.
 * Its clamp used to sit on the 124-byte limit and could land inside a 3-byte
 * character; this sweeps the cut points around that limit. */
static void check_title_clamp(void) {
    char src[400];
    size_t n = 0;
    for (size_t r = 0; r < 100; r++) {
        src[n++] = (char)0xE3;
        src[n++] = (char)0x81;
        src[n++] = (char)0x82; /* U+3042 */
    }
    src[n] = '\0';
    char clipped[NK_MAX_TITLE_LEN];
    for (size_t limit = 118; limit <= 140; limit++) {
        size_t kept = ui_clip_ellipsize_utf8(clipped, sizeof clipped, src, limit);
        size_t cap = NK_MAX_TITLE_LEN - 4;
        size_t want = oracle_kept(src, limit < cap ? limit : cap);
        if (!ellipsized_ok(clipped, sizeof clipped, kept, want)) {
            fprintf(stderr, "FAIL: title clamp cut at byte %zu: kept %zu, want %zu\n", limit, kept, want);
            g_failures++;
        }
    }
}

/* The last line of a wrapped paragraph (draw_text_wrapped_checked). */
static void check_wrapped_last_line(void) {
    char out[512];
    ui_clip_wrapped_last_line(out, sizeof out, "the quick brown fox jumps over");
    CHECK(strcmp(out, "the quick brown fox jumps o...") == 0,
          "a truncated wrapped last line keeps ... on ASCII text");

    ui_clip_wrapped_last_line(out, sizeof out, "ab\xF0\x9F\x98\x80");
    CHECK(strcmp(out, "ab...") == 0,
          "a wrapped last line ending in a 4-byte character keeps ... and a boundary");

    ui_clip_wrapped_last_line(out, sizeof out, "\xE3\x81\x82\xE3\x81\x84\xE3\x81\x86x");
    CHECK(strcmp(out, "\xE3\x81\x82\xE3\x81\x84...") == 0,
          "a wrapped last line cut inside a 3-byte character keeps ... and a boundary");

    ui_clip_wrapped_last_line(out, sizeof out, "\xC3\xA9\xC3\xA9");
    CHECK(strcmp(out, "...") == 0,
          "a wrapped last line cut inside a 2-byte character keeps ... and a boundary");

    ui_clip_wrapped_last_line(out, sizeof out, "h\xC3\xA9llo w\xC3\xB6rld");
    CHECK(strcmp(out, "h\xC3\xA9llo w\xC3\xB6...") == 0,
          "a wrapped last line with accents keeps its boundary and ...");

    for (size_t len = 4; len <= 40; len++) {
        char src[64];
        for (size_t i = 0; i < len; i++) src[i] = (char)('a' + (i % 26));
        src[len] = '\0';
        ui_clip_wrapped_last_line(out, sizeof out, src);
        bool ok = strlen(out) == len && memcmp(out, src, len - 3) == 0 &&
                  memcmp(out + len - 3, "...", 4) == 0;
        if (!ok) {
            fprintf(stderr, "FAIL: wrapped last line of %zu ASCII bytes lost its marker shape\n", len);
            g_failures++;
        }
    }

    ui_clip_wrapped_last_line(out, sizeof out, "ab");
    CHECK(strcmp(out, "ab") == 0, "a wrapped last line of three bytes or fewer is copied unchanged");
    ui_clip_wrapped_last_line(out, sizeof out, "");
    CHECK(out[0] == '\0', "an empty wrapped last line stays empty");
}

/* Bounded writes: the output window is surrounded by sentinel bytes, so any write
 * outside [0, out_size) shows up. Includes out_size 0 through 3, where the marker
 * cannot fit. */
static void check_bounded_output(void) {
    char src[64];
    size_t n = 2;
    src[0] = 'a';
    src[1] = 'b';
    for (size_t r = 0; r < 10; r++) {
        src[n++] = (char)0xF0;
        src[n++] = (char)0x9F;
        src[n++] = (char)0x98;
        src[n++] = (char)0x80; /* U+1F600 */
    }
    src[n] = '\0';

    unsigned char arena[48];
    for (size_t size = 0; size <= 20; size++) {
        memset(arena, 0xAA, sizeof arena);
        char *out = (char *)arena + 8;
        size_t kept = ui_clip_ellipsize_utf8(out, size, src, 40);
        bool ok = true;
        for (size_t i = 0; i < 8; i++) if (arena[i] != 0xAA) ok = false;
        for (size_t i = 8 + size; i < sizeof arena; i++) if (arena[i] != 0xAA) ok = false;
        if (size == 0) {
            ok = ok && kept == 0 && arena[8] == 0xAA;
        } else if (size < 4) {
            ok = ok && kept == 0 && out[0] == '\0';
        } else {
            size_t cap = size - 4;
            ok = ok && ellipsized_ok(out, size, kept, oracle_kept(src, cap < 40 ? cap : 40));
        }
        if (!ok) {
            fprintf(stderr, "FAIL: ellipsize into a %zu-byte buffer wrote outside it or lost its marker\n", size);
            g_failures++;
        }
    }

    /* The wrapped-last-line helper with the same tiny buffers. */
    const char *line = "ab\xF0\x9F\x98\x80" "cd";
    for (size_t size = 0; size <= 12; size++) {
        memset(arena, 0xAA, sizeof arena);
        char *out = (char *)arena + 8;
        ui_clip_wrapped_last_line(out, size, line);
        bool ok = true;
        for (size_t i = 0; i < 8; i++) if (arena[i] != 0xAA) ok = false;
        for (size_t i = 8 + size; i < sizeof arena; i++) if (arena[i] != 0xAA) ok = false;
        if (size == 0) {
            ok = ok && arena[8] == 0xAA;
        } else {
            size_t len = bounded_len(out, size);
            ok = ok && len < size && utf8_is_valid(out, len);
            if (size < 4) {
                ok = ok && len == 0;
            } else {
                ok = ok && len >= 3 && memcmp(out + len - 3, "...", 3) == 0;
            }
        }
        if (!ok) {
            fprintf(stderr, "FAIL: wrapped last line into a %zu-byte buffer wrote outside it or lost its marker\n", size);
            g_failures++;
        }
    }

    char tiny[8];
    CHECK(ui_clip_ellipsize_utf8(NULL, sizeof tiny, src, 3) == 0, "a NULL output buffer is ignored");
    CHECK(ui_clip_wrapped_last_line(NULL, sizeof tiny, line) == 0, "a NULL wrapped output buffer is ignored");
    CHECK(ui_clip_ellipsize_utf8(tiny, sizeof tiny, NULL, 3) == 0 && tiny[0] == '\0',
          "a NULL source gives an empty string");
}

/* The title word break scans back at most 24 bytes from a fitting prefix longer
 * than 12 bytes. A 13- to 23-byte prefix used to wrap the window's lower edge as a
 * size_t, so its scan never ran and a word was split. */
static void check_word_break(void) {
    const char *title = "abcdefghij klmnopqrstuvwxyz0123456789";
    for (size_t prefix = 13; prefix <= 35; prefix++) {
        size_t got = ui_clip_word_break(title, prefix);
        if (got != 10) {
            fprintf(stderr, "FAIL: a %zu-byte prefix breaks at %zu, not at the space (10)\n", prefix, got);
            g_failures++;
        }
    }
    /* The scan stops 24 bytes back and then tests the byte before its stop, so the
     * space at index 10 is reached from prefixes up to 35 and not from 36. */
    CHECK(ui_clip_word_break(title, 36) == 36, "a space more than 25 bytes back is not used");
    /* Short prefixes and NULL are never word-broken. */
    for (size_t prefix = 0; prefix <= 12; prefix++) {
        CHECK(ui_clip_word_break(title, prefix) == prefix, "a prefix of 12 bytes or fewer is kept");
    }
    CHECK(ui_clip_word_break(NULL, 20) == 20, "a NULL title keeps its prefix");
    /* No space at all: the prefix is kept whatever its length. */
    const char *solid = "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz";
    for (size_t prefix = 13; prefix <= 40; prefix++) {
        CHECK(ui_clip_word_break(solid, prefix) == prefix, "a title without a space keeps its prefix");
    }
    /* A space right at the cut drops only that space. */
    CHECK(ui_clip_word_break("abcdefghijklmn opq", 15) == 14, "a trailing space at the cut is dropped");
}

static void run_utf8_checks(void) {
    check_word_break();
    check_every_cut("2-byte", "\xC3\xA9", 40);
    check_every_cut("3-byte", "\xE3\x81\x82", 40);
    check_every_cut("4-byte", "\xF0\x9F\x98\x80", 40);
    check_title_clamp();
    check_wrapped_last_line();
    check_bounded_output();
}

int main(void) {
    run_utf8_checks();

    SDL_Surface *surface = SDL_CreateSurface(64, 64, SDL_PIXELFORMAT_RGBA8888);
    SDL_Renderer *ren = surface ? SDL_CreateSoftwareRenderer(surface) : NULL;
    if (!ren) {
        fprintf(stderr, "FAIL: software renderer unavailable: %s\n", SDL_GetError());
        return 1;
    }

    /* 1. No clip before the scope: none after it, and the next frame draws
          everywhere (the blank-window case). */
    {
        UiClipState saved = ui_clip_save(ren);
        SDL_Rect hero = { 10, 10, 20, 20 };
        SDL_SetRenderClipRect(ren, &hero);
        ui_clip_restore(ren, &saved);
        CHECK(!SDL_RenderClipEnabled(ren), "no clip is left enabled after restoring an unclipped state");
        CHECK(pixel_is_white_after_full_fill(ren, surface, 1, 1),
              "a full-target fill after the scope reaches a pixel outside the hero rectangle");
        CHECK(pixel_is_white_after_full_fill(ren, surface, 15, 15),
              "a full-target fill after the scope reaches a pixel inside the hero rectangle");
    }

    /* 2. An outer clip survives a nested scope unchanged. */
    {
        SDL_Rect outer = { 2, 2, 40, 40 };
        SDL_SetRenderClipRect(ren, &outer);
        UiClipState saved = ui_clip_save(ren);
        SDL_Rect inner = { 5, 5, 3, 3 };
        SDL_SetRenderClipRect(ren, &inner);
        ui_clip_restore(ren, &saved);
        SDL_Rect now = { 0, 0, 0, 0 };
        CHECK(SDL_RenderClipEnabled(ren), "the outer clip is still enabled after the nested scope");
        CHECK(SDL_GetRenderClipRect(ren, &now) && now.x == 2 && now.y == 2 && now.w == 40 && now.h == 40,
              "the outer clip rectangle is restored exactly");
        CHECK(pixel_is_white_after_full_fill(ren, surface, 30, 30), "the outer clip still admits its own area");
        CHECK(!pixel_is_white_after_full_fill(ren, surface, 50, 50), "the outer clip still excludes outside pixels");
        SDL_SetRenderClipRect(ren, NULL);
    }

    SDL_DestroyRenderer(ren);
    SDL_DestroySurface(surface);
    if (g_failures) {
        fprintf(stderr, "test_ui_clip: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("test_ui_clip: all checks passed\n");
    return 0;
}
