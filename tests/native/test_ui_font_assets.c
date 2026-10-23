// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <stdio.h>

static int report_failure(const char *reason)
{
    fprintf(stderr, "UI_FONT_SELFTEST: FAIL: %s: %s\n", reason, SDL_GetError());
    return 1;
}

int main(int argc, char **argv)
{
    static const Uint32 title_codepoints[] = {
        0x65e5, 0x672c, 0x8a9e, 0x30bf, 0x30a4, 0x30c8, 0x30eb
    };
    static const char japanese_title[] =
        "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x82\xbf\xe3\x82\xa4"
        "\xe3\x83\x88\xe3\x83\xab";
    int result = 1;
    bool sdl_ready = false;
    bool ttf_ready = false;
    TTF_TextEngine *text_engine = NULL;
    TTF_Font *japanese = NULL;
    TTF_Font *latin = NULL;
    TTF_Text *japanese_text = NULL;
    int hb_major = 0;
    int hb_minor = 0;
    int hb_patch = 0;

    if (argc != 3) {
        fprintf(stderr,
                "usage: %s <NotoSansJP-Regular.ttf> <ChakraPetch-SemiBold.ttf>\n",
                argv[0]);
        return 2;
    }
    if (!SDL_Init(0)) {
        return report_failure("SDL_Init failed");
    }
    sdl_ready = true;
    if (!TTF_Init()) {
        report_failure("TTF_Init failed");
        goto done;
    }
    ttf_ready = true;

    TTF_GetHarfBuzzVersion(&hb_major, &hb_minor, &hb_patch);
    if (hb_major == 0) {
        report_failure("SDL3_ttf was built without HarfBuzz");
        goto done;
    }
    text_engine = TTF_CreateSurfaceTextEngine();
    if (text_engine == NULL) {
        report_failure("could not create the SDL3_ttf text engine");
        goto done;
    }

    japanese = TTF_OpenFont(argv[1], 28.0f);
    if (japanese == NULL) {
        report_failure("could not open the pinned Noto Sans JP font");
        goto done;
    }
    int title_width = 0;
    int title_height = 0;
    japanese_text = TTF_CreateText(text_engine, japanese, japanese_title, 0);
    if (japanese_text == NULL || !TTF_GetTextSize(japanese_text, &title_width, &title_height) ||
        title_width <= 0 || title_height <= 0) {
        report_failure("Japanese title did not shape to a nonzero size");
        goto done;
    }
    for (size_t i = 0; i < sizeof(title_codepoints) / sizeof(title_codepoints[0]); ++i) {
        int minx = 0;
        int maxx = 0;
        int miny = 0;
        int maxy = 0;
        int advance = 0;
        if (!TTF_GetGlyphMetrics(japanese, title_codepoints[i],
                                 &minx, &maxx, &miny, &maxy, &advance) || advance <= 0) {
            report_failure("Japanese title contains an unavailable glyph or zero advance");
            goto done;
        }
    }

    /* Use a large point size so the pair adjustment survives integer pixel sizing. */
    latin = TTF_OpenFont(argv[2], 128.0f);
    if (latin == NULL) {
        report_failure("could not open the pinned Chakra Petch semibold font");
        goto done;
    }
    int minx_a = 0;
    int maxx_a = 0;
    int miny_a = 0;
    int maxy_a = 0;
    int minx_v = 0;
    int maxx_v = 0;
    int miny_v = 0;
    int maxy_v = 0;
    int advance_a = 0;
    int advance_v = 0;
    if (!TTF_GetGlyphMetrics(latin, 'A', &minx_a, &maxx_a, &miny_a, &maxy_a, &advance_a) ||
        !TTF_GetGlyphMetrics(latin, 'V', &minx_v, &maxx_v, &miny_v, &maxy_v, &advance_v) ||
        advance_a <= 0 || advance_v <= 0) {
        report_failure("Latin AV pair has a missing glyph or zero advance");
        goto done;
    }
    int av_width = 0;
    size_t av_length = 0;
    /* The shaped pair advance must include its GPOS kerning adjustment. */
    if (!TTF_MeasureString(latin, "AV", 2, 0, &av_width, &av_length) ||
        av_width <= 0 || av_length != 2 || av_width >= advance_a + advance_v) {
        fprintf(stderr, "UI_FONT_SELFTEST: AV pair advance=%d; separate advances=%d\n",
                av_width, advance_a + advance_v);
        report_failure("Latin AV shaping did not reduce the combined advance");
        goto done;
    }

    printf("UI_FONT_SELFTEST: PASS: Japanese title %dx%d; AV advance %d < separate advances %d; HarfBuzz %d.%d.%d\n",
           title_width, title_height, av_width, advance_a + advance_v,
           hb_major, hb_minor, hb_patch);
    result = 0;

done:
    if (japanese_text != NULL) {
        TTF_DestroyText(japanese_text);
    }
    if (latin != NULL) {
        TTF_CloseFont(latin);
    }
    if (japanese != NULL) {
        TTF_CloseFont(japanese);
    }
    if (text_engine != NULL) {
        TTF_DestroySurfaceTextEngine(text_engine);
    }
    if (ttf_ready) {
        TTF_Quit();
    }
    if (sdl_ready) {
        SDL_Quit();
    }
    return result;
}
