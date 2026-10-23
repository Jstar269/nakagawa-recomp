/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* nk_ui SDL_Renderer backend and SDL3_ttf text engine (lane C3b), "Layer 2" of the
   nk_ui test plan: the software renderer into an in-memory surface, probed pixel by
   pixel at 1x, 1.25x and 2x. Snapshots are written as BMPs for review and are never
   compared byte for byte, because the SDL3 and FreeType builds float.

   Exit 77 means SKIP, with the reason on stdout: the build lacked the SDL3 or
   SDL3_ttf headers, the SDL3_ttf shared library cannot be loaded, or a fixture font
   is missing. Run from the repository root. */

#include "nk_ui.h"
#include "nk_ui_draw.h"
#include "nk_ui_theme.h"
#include "nk_ui_waves.h"
#include "backend/nk_ui_sdlr.h"

#include <stdio.h>
#include <string.h>

#ifndef SR_SELFTEST_BUILD_ROOT
#define SR_SELFTEST_BUILD_ROOT "build"
#endif

#define EXIT_SKIP 77

#if !defined(NK_UI_WITH_SDL) || !defined(NK_UI_WITH_SDL_TTF)

int main(void) {
    printf("SKIP nk_ui_sdlr: SDL3 and SDL3_ttf headers were not available at build time "
           "(NK_UI_WITH_SDL and NK_UI_WITH_SDL_TTF not defined)\n");
    return EXIT_SKIP;
}

#else

#include <SDL3/SDL.h>

#include <limits.h>
#include <math.h>

static int g_failures = 0;

#define CHECK(condition, message) \
    do { \
        if (!(condition)) { \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
            ++g_failures; \
            return 0; \
        } \
    } while (0)

#define GUDEA_PATH "fixtures/fonts/gudea/Gudea-Regular.ttf"
#define MPLUS_PATH "fixtures/fonts/mplus1p/MPLUS1p-Regular.ttf"

/* Hosts the same two faces nk_ui labels will use: font 0 is the Latin body face
   with the Japanese face as its per-glyph fallback, font 1 is the Japanese face. */
static NkUiTtf *g_ttf;

static const char kJapaneseTitle[] = "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"
                                     "\xE3\x81\xAE"
                                     "\xE3\x82\xBF\xE3\x82\xA4\xE3\x83\x88\xE3\x83\xAB";
static const uint32_t kJapaneseCodepoints[] = {0x65E5u, 0x672Cu, 0x8A9Eu, 0x306Eu,
                                               0x30BFu, 0x30A4u, 0x30C8u, 0x30EBu};

static bool file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        return false;
    }
    fclose(file);
    return true;
}

/* ---- text engine ---- */

static int test_ttf_measure_and_kerning(void) {
    NkUiTextBackend text = nk_ui_ttf_backend(g_ttf);
    NkUiTextSize a16 = text.measure(text.user, 0, 16, "A", 1);
    NkUiTextSize a32 = text.measure(text.user, 0, 32, "A", 1);
    NkUiTextSize v32 = text.measure(text.user, 0, 32, "V", 1);
    NkUiTextSize av32 = text.measure(text.user, 0, 32, "AV", 2);
    NkUiTextSize empty = text.measure(text.user, 0, 32, "", 0);
    NkUiGlyph glyphs[8];
    int count;
    CHECK(a16.width > 0.0f && a32.width > a16.width, "advance grows with the pixel size");
    CHECK(a32.height > 0.0f && a32.baseline > 0.0f && a32.baseline < a32.height,
          "line height and baseline are sane");
    CHECK(empty.width == 0.0f && empty.height == a32.height,
          "an empty string has no width and the face's line height");
    CHECK(av32.width < a32.width + v32.width,
          "the AV pair is kerned: shaped width is under the sum of the advances");
    count = text.shape(text.user, 0, 32, "AV", 2, glyphs, 8);
    CHECK(count == 2, "two glyphs for AV");
    CHECK(glyphs[1].x < a32.width, "the kerned V starts before the end of A's advance");
    CHECK(glyphs[0].codepoint == 'A' && glyphs[1].codepoint == 'V', "codepoints follow the text");
    CHECK(glyphs[0].x < glyphs[1].x, "glyphs advance left to right");
    CHECK(glyphs[0].advance_x > 0.0f && glyphs[1].advance_x > 0.0f, "advances are positive");
    CHECK(text.shape(text.user, 0, 32, "AV", 2, glyphs, 1) == 1, "shape honours max");
    CHECK(text.shape(text.user, 0, 32, "", 0, glyphs, 8) == 0, "shape of empty is empty");
    CHECK(text.shape(text.user, 5, 32, "A", 1, glyphs, 8) == 0, "unregistered font shapes nothing");
    return 1;
}

static int test_ttf_japanese_shaping(void) {
    NkUiTextBackend text = nk_ui_ttf_backend(g_ttf);
    NkUiGlyph glyphs[16];
    NkUiGlyphBitmap bitmap;
    NkUiTextSize size;
    int count;
    int i;
    /* The body face has no kana or kanji; every one of these glyphs must come from
       the fallback face (depth 1), with a real glyph index. */
    count = text.shape(text.user, 0, 24, kJapaneseTitle, sizeof(kJapaneseTitle) - 1u, glyphs, 16);
    CHECK(count == 8, "one glyph per Japanese character");
    for (i = 0; i < count; ++i) {
        CHECK(glyphs[i].codepoint == kJapaneseCodepoints[i], "codepoint order matches the title");
        CHECK((glyphs[i].glyph >> 24) == 1u, "Japanese glyphs come from the fallback face");
        CHECK((glyphs[i].glyph & 0x00FFFFFFu) != 0u, "no .notdef glyph in the title");
        if (i > 0) {
            CHECK(glyphs[i].x > glyphs[i - 1].x, "Japanese glyphs advance left to right");
        }
        memset(&bitmap, 0, sizeof(bitmap));
        CHECK(text.raster(text.user, 0, 24, glyphs[i].glyph, &bitmap), "rasterise a Japanese glyph");
        CHECK(bitmap.width > 0 && bitmap.height > 0 && bitmap.pixels != NULL &&
              bitmap.pitch >= bitmap.width, "a Japanese glyph has a coverage bitmap");
        CHECK(bitmap.bearing_y == bitmap.height && bitmap.bearing_x == 0,
              "bitmap origin convention");
    }
    size = text.measure(text.user, 0, 24, kJapaneseTitle, sizeof(kJapaneseTitle) - 1u);
    CHECK(fabsf(size.width - 8.0f * 24.0f) <= 16.0f,
          "eight full-width characters measure about eight ems");
    /* The Japanese face alone agrees with the fallback path. */
    count = text.shape(text.user, 1, 24, kJapaneseTitle, sizeof(kJapaneseTitle) - 1u, glyphs, 16);
    CHECK(count == 8 && (glyphs[0].glyph >> 24) == 0u, "the Japanese face is depth 0 for itself");
    /* Mixed Latin and Japanese: the Latin letters stay in the body face. */
    count = text.shape(text.user, 0, 24, "A\xE6\x97\xA5\xE6\x9C\xAC V", 9, glyphs, 16);
    CHECK(count == 4, "A, two kanji and V (the space draws nothing)");
    CHECK((glyphs[0].glyph >> 24) == 0u && (glyphs[1].glyph >> 24) == 1u &&
          (glyphs[2].glyph >> 24) == 1u && (glyphs[3].glyph >> 24) == 0u,
          "per-glyph fallback depths for mixed text");
    CHECK(glyphs[3].codepoint == 'V', "V keeps its codepoint after wide glyphs");
    CHECK(!text.raster(text.user, 0, 24, (3u << 24) | 5u, &bitmap),
          "a fallback depth the font does not have is refused");
    return 1;
}

/* ---- rendering ---- */

typedef struct {
    SDL_Surface *surface;
    SDL_Renderer *renderer;
} Target;

static bool target_open(Target *target, int w, int h) {
    memset(target, 0, sizeof(*target));
    target->surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    if (target->surface == NULL) {
        return false;
    }
    target->renderer = SDL_CreateSoftwareRenderer(target->surface);
    if (target->renderer == NULL) {
        SDL_DestroySurface(target->surface);
        target->surface = NULL;
        return false;
    }
    return true;
}

static void target_close(Target *target) {
    if (target->renderer != NULL) {
        SDL_DestroyRenderer(target->renderer);
    }
    if (target->surface != NULL) {
        SDL_DestroySurface(target->surface);
    }
    memset(target, 0, sizeof(*target));
}

static void target_clear(Target *target, uint32_t rgba) {
    SDL_SetRenderDrawBlendMode(target->renderer, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(target->renderer, (Uint8)(rgba >> 24), (Uint8)(rgba >> 16),
                           (Uint8)(rgba >> 8), (Uint8)rgba);
    SDL_RenderClear(target->renderer);
}

/* Reads one pixel as 0xRRGGBBAA after flushing pending renderer work. */
static uint32_t target_pixel(Target *target, int x, int y) {
    const uint8_t *p;
    SDL_FlushRenderer(target->renderer);
    p = (const uint8_t *)target->surface->pixels + (size_t)y * (size_t)target->surface->pitch +
        (size_t)x * 4u;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t target_hash(Target *target) {
    const uint8_t *p;
    uint64_t h = 1469598103934665603ull;
    size_t y;
    SDL_FlushRenderer(target->renderer);
    for (y = 0; y < (size_t)target->surface->h; ++y) {
        size_t x;
        p = (const uint8_t *)target->surface->pixels + y * (size_t)target->surface->pitch;
        for (x = 0; x < (size_t)target->surface->w * 4u; ++x) {
            h = (h ^ p[x]) * 1099511628211ull;
        }
    }
    return h;
}

static int color_close(uint32_t a, uint32_t b, int tolerance) {
    int shift;
    for (shift = 24; shift >= 8; shift -= 8) {
        int ca = (int)((a >> shift) & 0xFFu);
        int cb = (int)((b >> shift) & 0xFFu);
        if (ca - cb > tolerance || cb - ca > tolerance) {
            return 0;
        }
    }
    return 1;
}

static void save_snapshot(Target *target, const char *name) {
    char path[256];
    SDL_FlushRenderer(target->renderer);
    snprintf(path, sizeof(path), "%s/nk_ui_sdlr_%s.bmp", SR_SELFTEST_BUILD_ROOT, name);
    if (!SDL_SaveBMP(target->surface, path)) {
        printf("note: could not write %s: %s\n", path, SDL_GetError());
    }
}

/* Builds the synthetic screen: a panel with a title, a Japanese title, a card that
   stands in for the focus ring (3 px accent border; the real ring is lane C4's), a
   kerning sample and a primary button. */
static const NkUiDrawList *build_screen(NkUi *ui, int pixel_w, int pixel_h, int scale_pct) {
    NkUiViewport viewport;
    NkUiLayout root;
    NkUiLayout card;
    NkUiLayout row;
    NkUiStyle root_style;
    NkUiStyle card_style;
    const NkUiTheme *theme = nk_ui_theme_n1();
    memset(&viewport, 0, sizeof(viewport));
    viewport.pixel_w = pixel_w;
    viewport.pixel_h = pixel_h;
    viewport.display_scale = 1.0f;
    viewport.ui_scale_pct = scale_pct;
    viewport.text_size_pct = 100;
    nk_ui_set_viewport(ui, &viewport);
    memset(&root, 0, sizeof(root));
    root.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    root.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    root.direction = NK_UI_LAYOUT_COLUMN;
    root.gap = 10.0f;
    root.padding_left = root.padding_top = root.padding_right = root.padding_bottom = 16.0f;
    memset(&root_style, 0, sizeof(root_style));
    root_style.fill_rgba = theme->surface;
    memset(&card, 0, sizeof(card));
    card.width = (NkUiSizing){NK_UI_SIZE_FIXED, 200.0f, 0.0f, 0.0f};
    card.height = (NkUiSizing){NK_UI_SIZE_FIXED, 48.0f, 0.0f, 0.0f};
    card.direction = NK_UI_LAYOUT_ROW;
    card.padding_left = card.padding_top = card.padding_right = card.padding_bottom = 8.0f;
    memset(&card_style, 0, sizeof(card_style));
    card_style.fill_rgba = theme->surface_raised;
    card_style.border_rgba = theme->accent_dark;
    card_style.border_width = 3.0f;
    card_style.corner_radius = 6.0f;
    memset(&row, 0, sizeof(row));
    row.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    row.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    row.direction = NK_UI_LAYOUT_ROW;
    nk_ui_begin(ui, 37.0);
    if (!nk_ui_begin_container(ui, 1u, &root, &root_style)) {
        return NULL;
    }
    nk_ui_label(ui, 2u, "Nakagawa Recomp", 0, 28);
    nk_ui_label(ui, 3u, kJapaneseTitle, 0, 20);
    if (nk_ui_begin_container(ui, 4u, &card, &card_style)) {
        nk_ui_label(ui, 5u, "AVAT To", 0, 18);
        nk_ui_end_container(ui);
    }
    if (nk_ui_begin_container(ui, 6u, &row, NULL)) {
        (void)nk_ui_button(ui, 7u, "PLAY", NK_UI_BUTTON_PRIMARY);
        nk_ui_end_container(ui);
    }
    nk_ui_end_container(ui);
    return nk_ui_end(ui);
}

typedef struct {
    int min_x;
    int min_y;
    int max_x;
    int max_y;
    int pixels;
} Ink;

/* Pixels in `box` that differ from `background` by more than `threshold`. */
static Ink measure_ink(Target *target, const int box[4], uint32_t background, int threshold) {
    Ink ink = {INT_MAX, INT_MAX, -1, -1, 0};
    int x;
    int y;
    for (y = box[1]; y < box[3]; ++y) {
        for (x = box[0]; x < box[2]; ++x) {
            if (x < 0 || y < 0 || x >= target->surface->w || y >= target->surface->h) {
                continue;
            }
            if (!color_close(target_pixel(target, x, y), background, threshold)) {
                ++ink.pixels;
                if (x < ink.min_x) ink.min_x = x;
                if (y < ink.min_y) ink.min_y = y;
                if (x > ink.max_x) ink.max_x = x;
                if (y > ink.max_y) ink.max_y = y;
            }
        }
    }
    return ink;
}

typedef struct {
    int ink_height_of_title;   /* first (28 px) label's ink height, for scale checks */
} ScaleResult;

static int probe_frame(Target *target, const NkUiDrawList *list, uint32_t page, ScaleResult *result,
                       const char *label) {
    const NkUiTheme *theme = nk_ui_theme_n1();
    uint32_t i;
    int texts = 0;
    int accent_strips = 0;
    (void)label;
    for (i = 0; i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_BORDER && cmd->rgba_pm == theme->accent_dark &&
            cmd->bounds[2] - cmd->bounds[0] != cmd->bounds[3] - cmd->bounds[1]) {
            /* The focus-style border is opaque accent along its straight strips
               (the square commands are its antialiased corner arcs). */
            int cx = (cmd->bounds[0] + cmd->bounds[2]) / 2;
            int cy = (cmd->bounds[1] + cmd->bounds[3]) / 2;
            CHECK(color_close(target_pixel(target, cx, cy), theme->accent_dark, 1),
                  "border strip centre is the accent colour");
            ++accent_strips;
        } else if (cmd->kind == NK_UI_DRAW_TEXT) {
            const NkUiTextRun *run = (const NkUiTextRun *)cmd->custom;
            int box[4];
            int slack = run->size_px / 6 > 3 ? run->size_px / 6 : 3;
            Ink inside;
            Ink around;
            uint32_t bg = page;
            CHECK(run != NULL && run->length > 0, "text command carries its run");
            /* The background under a label: the panel, the card, or the button. */
            if (strncmp(run->text, "AVAT", 4) == 0) {
                bg = theme->surface_raised;
            } else if (strncmp(run->text, "PLAY", 4) == 0) {
                bg = theme->accent_dark;
            }
            box[0] = cmd->bounds[0];
            box[1] = cmd->bounds[1];
            box[2] = cmd->bounds[2];
            box[3] = cmd->bounds[3];
            inside = measure_ink(target, box, bg, 40);
            CHECK(inside.pixels >= (int)run->length * 3, "text pixels exist inside the text rect");
            box[0] -= slack;
            box[1] -= slack;
            box[2] += slack;
            box[3] += slack;
            around = measure_ink(target, box, bg, 40);
            CHECK(around.min_x >= cmd->bounds[0] - 3 && around.max_x <= cmd->bounds[2] + 3,
                  "text ink stays within the rect horizontally");
            CHECK(around.min_y >= cmd->bounds[1] - slack && around.max_y <= cmd->bounds[3] + slack,
                  "text ink stays near the rect vertically");
            if (strncmp(run->text, "Nakagawa", 8) == 0) {
                result->ink_height_of_title = inside.max_y - inside.min_y + 1;
            }
            ++texts;
        }
    }
    CHECK(accent_strips >= 4, "the accent border draws as straight strips");
    CHECK(texts == 4, "four text commands in the screen");
    return 1;
}

static int test_snapshots_and_atlas(void) {
    static const struct {
        int pct;
        float density;
        const char *name;
    } scales[] = {{100, 1.0f, "1x"}, {125, 1.25f, "1_25x"}, {200, 2.0f, "2x"}};
    NkUiTextBackend text = nk_ui_ttf_backend(g_ttf);
    NkUiConfig config;
    NkUi *ui;
    NkUiSdlrStats stats;
    int title_height[3] = {0, 0, 0};
    size_t s;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the ui with the SDL3_ttf backend");
    for (s = 0; s < 3u; ++s) {
        Target target;
        NkUiSdlr *backend;
        const NkUiDrawList *list;
        ScaleResult result = {0};
        uint64_t first_hash;
        uint32_t uploads;
        int w = (int)floorf(360.0f * scales[s].density + 0.5f);
        int h = (int)floorf(220.0f * scales[s].density + 0.5f);
        CHECK(target_open(&target, w, h), "open a software render target");
        backend = nk_ui_sdlr_create(target.renderer, &text);
        CHECK(backend != NULL, "create the SDL renderer backend");
        nk_ui_sdlr_set_scale(backend, scales[s].density);
        list = build_screen(ui, w, h, scales[s].pct);
        CHECK(list != NULL, "build the synthetic screen");
        target_clear(&target, nk_ui_theme_n1()->surface);
        CHECK(nk_ui_sdlr_render(backend, list), "render the list");
        CHECK(probe_frame(&target, list, nk_ui_theme_n1()->surface, &result, scales[s].name),
              "probes pass");
        title_height[s] = result.ink_height_of_title;
        save_snapshot(&target, scales[s].name);
        nk_ui_sdlr_get_stats(backend, &stats);
        CHECK(stats.atlas_pages == 1u && stats.atlas_glyphs > 10u && stats.atlas_uploads > 10u,
              "the first frame fills one atlas page");
        CHECK(stats.text_runs_drawn == 4u && stats.glyphs_drawn > 10u, "all text runs were drawn");
        first_hash = target_hash(&target);
        uploads = stats.atlas_uploads;
        /* Same list, same scale: pure cache hits and identical pixels. */
        target_clear(&target, nk_ui_theme_n1()->surface);
        CHECK(nk_ui_sdlr_render(backend, list), "render the list again");
        nk_ui_sdlr_get_stats(backend, &stats);
        CHECK(stats.atlas_uploads == uploads && stats.atlas_rebuilds == 0u,
              "a second frame uploads nothing and rebuilds nothing");
        CHECK(target_hash(&target) == first_hash, "rendering is deterministic");
        nk_ui_sdlr_destroy(backend);
        target_close(&target);
    }
    CHECK(title_height[0] > 10, "the 1x title has visible ink");
    CHECK(title_height[1] >= title_height[0] && title_height[2] > title_height[1],
          "ink height grows with the scale");
    CHECK(title_height[2] >= title_height[0] * 2 - 4 && title_height[2] <= title_height[0] * 2 + 6,
          "2x ink is about twice the 1x height");
    nk_ui_destroy(ui);
    return 1;
}

static int test_atlas_rebuild_on_scale_change(void) {
    NkUiTextBackend text = nk_ui_ttf_backend(g_ttf);
    NkUiConfig config;
    NkUi *ui;
    Target target;
    NkUiSdlr *backend;
    NkUiSdlrStats stats;
    const NkUiDrawList *list;
    uint32_t glyphs_at_1x;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the ui");
    CHECK(target_open(&target, 720, 440), "open a render target");
    backend = nk_ui_sdlr_create(target.renderer, &text);
    CHECK(backend != NULL, "create the backend");
    nk_ui_sdlr_set_scale(backend, 1.0f);
    list = build_screen(ui, 360, 220, 100);
    CHECK(list != NULL && nk_ui_sdlr_render(backend, list), "render at 1x");
    nk_ui_sdlr_get_stats(backend, &stats);
    glyphs_at_1x = stats.atlas_glyphs;
    CHECK(stats.atlas_pages == 1u && glyphs_at_1x > 10u && stats.atlas_rebuilds == 0u,
          "1x populates one page without a rebuild");
    nk_ui_sdlr_set_scale(backend, 1.0f);
    nk_ui_sdlr_get_stats(backend, &stats);
    CHECK(stats.atlas_pages == 1u && stats.atlas_rebuilds == 0u,
          "an unchanged scale keeps the atlas");
    nk_ui_sdlr_set_scale(backend, 2.0f);
    nk_ui_sdlr_get_stats(backend, &stats);
    CHECK(stats.atlas_pages == 0u && stats.atlas_glyphs == 0u && stats.atlas_rebuilds == 1u,
          "a scale change drops every page");
    list = build_screen(ui, 720, 440, 200);
    target_clear(&target, nk_ui_theme_n1()->surface);
    CHECK(list != NULL && nk_ui_sdlr_render(backend, list), "render at 2x");
    nk_ui_sdlr_get_stats(backend, &stats);
    CHECK(stats.atlas_pages == 1u && stats.atlas_glyphs > 10u,
          "the atlas is rebuilt lazily at the new size");
    nk_ui_sdlr_destroy(backend);
    target_close(&target);
    nk_ui_destroy(ui);
    return 1;
}

static int test_atlas_page_overflow(void) {
    /* A glyph the size of a quarter page, in many distinct pixel sizes, forces the
       atlas past its page limit; the backend must rebuild instead of failing. */
    NkUiTextBackend text = nk_ui_ttf_backend(g_ttf);
    Target target;
    NkUiSdlr *backend;
    NkUiSdlrStats stats;
    NkUiDrawBuffer buffer;
    NkUiPixelRect bounds = {0, 0, 800, 480};
    int px;
    int runs = 0;
    memset(&buffer, 0, sizeof(buffer));
    CHECK(target_open(&target, 800, 480), "open a render target");
    backend = nk_ui_sdlr_create(target.renderer, &text);
    CHECK(backend != NULL, "create the backend");
    for (px = 300; px < 420; px += 3) {
        CHECK(nk_ui_draw_add_text(&buffer, bounds,
                                  "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", 9, 0, px,
                                  0xFFFFFFFFu, 1u), "queue a large text run");
        ++runs;
    }
    target_clear(&target, 0x000000FFu);
    CHECK(nk_ui_sdlr_render(backend, &buffer.list), "render past the page limit");
    nk_ui_sdlr_get_stats(backend, &stats);
    CHECK(stats.atlas_rebuilds >= 1u, "the atlas rebuilt when it ran out of pages");
    CHECK(stats.atlas_pages <= 8u && stats.text_runs_drawn == (uint32_t)runs,
          "every run was still drawn");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_sdlr_destroy(backend);
    target_close(&target);
    return 1;
}

static int test_scissor_and_custom(void) {
    Target target;
    NkUiSdlr *backend;
    NkUiSdlrStats stats;
    NkUiDrawBuffer buffer;
    NkUiWavesCommand waves;
    NkUiWavePalette palette;
    const uint32_t red = 0xFF0000FFu;
    const uint32_t blue = 0x0000FFFFu;
    const uint32_t black = 0x000000FFu;
    memset(&buffer, 0, sizeof(buffer));
    memset(&waves, 0, sizeof(waves));
    CHECK(nk_ui_waves_palette(0u, &palette), "base wave palette");
    nk_ui_waves_command(37.0, 32.0f, 32.0f, &palette, &waves);
    CHECK(target_open(&target, 64, 64), "open a render target");
    backend = nk_ui_sdlr_create(target.renderer, NULL);
    CHECK(backend != NULL, "create a backend with no text backend");
    CHECK(nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){10, 10, 30, 30}, 1u), "scissor on");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){0.0f, 0.0f, 64.0f, 64.0f}, 1.0f, 0.0f, red, 2u),
          "clipped red rect");
    CHECK(nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){10, 10, 30, 30}, 1u), "scissor off");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){40.0f, 40.0f, 10.0f, 10.0f}, 1.0f, 0.0f, blue, 3u),
          "unclipped blue rect");
    CHECK(nk_ui_draw_add_text(&buffer, (NkUiPixelRect){0, 0, 20, 20}, "A", 1, 0, 16, 0xFFFFFFFFu, 4u),
          "text with a backend that cannot draw it");
    CHECK(nk_ui_draw_add_custom(&buffer, NK_UI_CUSTOM_WAVES, &waves, (NkUiPixelRect){48, 0, 64, 16}, 5u),
          "waves custom command");
    CHECK(nk_ui_draw_add_custom(&buffer, NK_UI_CUSTOM_NONE + 7u, &waves, (NkUiPixelRect){0, 0, 4, 4}, 6u),
          "unknown custom command");
    target_clear(&target, black);
    CHECK(nk_ui_sdlr_render(backend, &buffer.list), "render the scissor list");
    CHECK(target_pixel(&target, 20, 20) == red, "inside the scissor is drawn");
    CHECK(target_pixel(&target, 5, 5) == black, "outside the scissor is clipped");
    CHECK(target_pixel(&target, 30, 20) == black && target_pixel(&target, 20, 30) == black,
          "the scissor's far edges are exclusive");
    CHECK(target_pixel(&target, 45, 45) == blue, "drawing after the scissor ends is unclipped");
    CHECK(target_pixel(&target, 2, 2) == black, "no text without a text backend");
    nk_ui_sdlr_get_stats(backend, &stats);
    CHECK(stats.custom_skipped == 1u, "an unknown custom command is skipped and counted");
    CHECK(stats.waves_drawn == 1u && stats.waves_rendered == 1u,
          "the waves command is drawn through the CPU reference");
    CHECK(!SDL_RenderClipEnabled(target.renderer), "the renderer's clip state is restored");
    CHECK(!nk_ui_sdlr_render(backend, NULL) && !nk_ui_sdlr_render(NULL, &buffer.list),
          "NULL arguments are refused");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_sdlr_destroy(backend);
    target_close(&target);
    return 1;
}

static int test_rounded_corner_fringe(void) {
    Target target;
    NkUiSdlr *backend;
    NkUiDrawBuffer buffer;
    uint32_t corner;
    uint32_t center;
    memset(&buffer, 0, sizeof(buffer));
    CHECK(target_open(&target, 64, 64), "open a render target");
    backend = nk_ui_sdlr_create(target.renderer, NULL);
    CHECK(backend != NULL, "create the backend");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){8.0f, 8.0f, 48.0f, 48.0f}, 1.0f, 12.0f,
                              0xFFFFFFFFu, 1u), "rounded white rect");
    target_clear(&target, 0x000000FFu);
    CHECK(nk_ui_sdlr_render(backend, &buffer.list), "render the rounded rect");
    center = target_pixel(&target, 32, 32);
    corner = target_pixel(&target, 8, 8);
    CHECK(center == 0xFFFFFFFFu, "the middle is solid");
    CHECK(corner == 0x000000FFu, "the cut corner stays background");
    /* The fringe fades from white to black without a dark halo: a pixel on the
       curve is a grey no darker than the background and no brighter than white. */
    {
        uint32_t edge = target_pixel(&target, 10, 10);
        int r = (int)(edge >> 24);
        CHECK(r >= 0 && r <= 255, "fringe pixel is a valid grey");
        CHECK(((edge >> 16) & 0xFFu) == (uint32_t)r && ((edge >> 8) & 0xFFu) == (uint32_t)r,
              "fringe pixels stay neutral grey: premultiplied blending, no colour halo");
    }
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_sdlr_destroy(backend);
    target_close(&target);
    return 1;
}

static int test_rounded_border_and_waves(void) {
    Target target;
    NkUiSdlr *backend;
    NkUiSdlrStats stats;
    NkUiDrawBuffer buffer;
    NkUiWavesCommand waves;
    NkUiWavePalette palette;
    const float radii[4] = {12.0f, 12.0f, 12.0f, 12.0f};
    uint32_t brightest = 0u;
    int y;
    memset(&buffer, 0, sizeof(buffer));
    CHECK(target_open(&target, 64, 64), "open a render target");
    backend = nk_ui_sdlr_create(target.renderer, NULL);
    CHECK(backend != NULL, "create the backend");
    /* A rounded 3 px border: straight runs between the corners, a quarter ring
       at each corner, and the inside left alone. */
    CHECK(nk_ui_draw_add_border_corners(&buffer, (NkUiRect){8.0f, 8.0f, 48.0f, 48.0f}, 1.0f,
                                        3.0f, radii, 0xFFFFFFFFu, 1u),
          "rounded white border");
    target_clear(&target, 0x000000FFu);
    CHECK(nk_ui_sdlr_render(backend, &buffer.list), "render the rounded border");
    CHECK(target_pixel(&target, 32, 9) == 0xFFFFFFFFu, "the top run is solid");
    CHECK(target_pixel(&target, 9, 32) == 0xFFFFFFFFu, "the left run is solid");
    CHECK(target_pixel(&target, 32, 32) == 0x000000FFu, "the inside stays clear");
    CHECK(target_pixel(&target, 8, 8) == 0x000000FFu, "the cut corner stays background");
    CHECK(target_pixel(&target, 12, 12) != 0x000000FFu, "the corner ring is drawn");
    nk_ui_draw_buffer_reset(&buffer);

    /* The waves: drawn through the CPU reference, brightest in the lower part,
       capped, and not re-rendered for an unchanged command. */
    CHECK(nk_ui_waves_palette(0u, &palette), "base wave palette");
    nk_ui_waves_command(37.0, 64.0f, 64.0f, &palette, &waves);
    CHECK(nk_ui_draw_add_custom(&buffer, NK_UI_CUSTOM_WAVES, &waves,
                                (NkUiPixelRect){0, 0, 64, 64}, 2u), "waves command");
    target_clear(&target, 0x000000FFu);
    CHECK(nk_ui_sdlr_render(backend, &buffer.list), "render the waves");
    CHECK(nk_ui_sdlr_render(backend, &buffer.list), "render the same waves again");
    for (y = 0; y < 64; ++y) {
        uint32_t pixel = target_pixel(&target, 32, y);
        if ((pixel >> 16 & 0xFFu) > (brightest >> 16 & 0xFFu)) brightest = pixel;
    }
    CHECK(((brightest >> 16) & 0xFFu) > 0u, "the waves add light over the background");
    CHECK(((brightest >> 16) & 0xFFu) < 0x60u, "the waves stay dim (alpha cap)");
    CHECK(target_pixel(&target, 32, 1) == 0x000000FFu, "the top of the window stays clear");
    nk_ui_sdlr_get_stats(backend, &stats);
    CHECK(stats.waves_drawn == 2u && stats.waves_rendered == 1u,
          "an unchanged waves command reuses its texture");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_sdlr_destroy(backend);
    target_close(&target);
    return 1;
}

static int test_availability(void) {
    CHECK(nk_ui_sdlr_available() && nk_ui_ttf_available(), "this build includes both halves");
    CHECK(nk_ui_sdlr_create(NULL, NULL) == NULL, "no renderer, no backend");
    CHECK(!nk_ui_ttf_add_font(NULL, 0, GUDEA_PATH), "add_font on NULL is refused");
    return 1;
}

int main(void) {
    static const char *const fixtures[] = {GUDEA_PATH, MPLUS_PATH};
    size_t i;
    int passed = 0;
    int total = 0;
    for (i = 0; i < sizeof(fixtures) / sizeof(fixtures[0]); ++i) {
        if (!file_exists(fixtures[i])) {
            printf("FAIL fixture font %s is missing (run from the repository root)\n", fixtures[i]);
            return 1;
        }
    }
    g_ttf = nk_ui_ttf_create(NULL, 0);
    if (g_ttf == NULL) {
        printf("SKIP nk_ui_sdlr: SDL3_ttf is not usable here: %s\n", nk_ui_ttf_last_error());
        return EXIT_SKIP;
    }
    /* Slot 6 is the design system's label face (button labels); the fixture
       Latin face stands in for it here. */
    if (!nk_ui_ttf_add_font(g_ttf, 0, GUDEA_PATH) || !nk_ui_ttf_add_font(g_ttf, 1, MPLUS_PATH) ||
        !nk_ui_ttf_add_font(g_ttf, NK_UI_FONT_MONO_MEDIUM, GUDEA_PATH) ||
        !nk_ui_ttf_set_fallback(g_ttf, 0, 1)) {
        printf("FAIL registering fixture fonts: %s\n", nk_ui_ttf_last_error());
        nk_ui_ttf_destroy(g_ttf);
        return 1;
    }
#define RUN(test) \
    do { \
        ++total; \
        if (test()) { \
            ++passed; \
            printf("PASS %s\n", #test); \
        } \
    } while (0)
    RUN(test_availability);
    RUN(test_ttf_measure_and_kerning);
    RUN(test_ttf_japanese_shaping);
    RUN(test_scissor_and_custom);
    RUN(test_rounded_corner_fringe);
    RUN(test_rounded_border_and_waves);
    RUN(test_snapshots_and_atlas);
    RUN(test_atlas_rebuild_on_scale_change);
    RUN(test_atlas_page_overflow);
#undef RUN
    nk_ui_ttf_destroy(g_ttf);
    printf("nk_ui_sdlr: %d of %d tests passed\n", passed, total);
    return g_failures == 0 && passed == total ? 0 : 1;
}

#endif
