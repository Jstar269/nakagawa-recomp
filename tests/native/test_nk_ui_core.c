/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui.h"
#include "nk_ui_draw.h"
#include "nk_ui_text.h"
#include "nk_ui_theme.h"
#include "nk_ui_waves.h"
#include "backend/nk_ui_sdlr.h"
#include "backend/nk_ui_vk.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s\n", message); \
        return 0; \
    } \
} while (0)

static const char *const g_ui_core_files[] = {
    "src/ui/nk_ui.h",
    "src/ui/nk_ui.c",
    "src/ui/nk_ui_clay.c",
    "src/ui/nk_ui_theme.h",
    "src/ui/nk_ui_theme.c",
    "src/ui/nk_ui_draw.h",
    "src/ui/nk_ui_draw.c",
    "src/ui/nk_ui_text.h",
    "src/ui/nk_ui_text.c",
    "src/ui/nk_ui_widgets.h",
    "src/ui/nk_ui_widgets.c",
    "src/ui/nk_ui_waves.h",
    "src/ui/nk_ui_waves.c",
    "src/ui/nk_ui_dump.c",
    "src/ui/nk_ui_focus.h",
    "src/ui/nk_ui_focus.c",
    "src/ui/nk_ui_input.h",
    "src/ui/nk_ui_input.c",
    "src/ui/nk_ui_prompts.c",
    "src/ui/nk_ui_sdl_input.c",
    /* nk_ui_ttf.c and backend/nk_ui_sdlr.* are the SDL edge since lane C3b: they
       include SDL headers behind NK_UI_WITH_SDL and are covered by test_nk_ui_sdlr.c. */
    "src/ui/backend/nk_ui_vk.h",
    "src/ui/backend/nk_ui_vk.c"
};

static int contains_ascii_case_insensitive(const char *text, const char *needle) {
    size_t needle_length = strlen(needle);
    const char *start;
    for (start = text; *start != '\0'; ++start) {
        size_t i;
        for (i = 0; i < needle_length; ++i) {
            char a = start[i];
            char b = needle[i];
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a == '\0' || a != b) break;
        }
        if (i == needle_length) return 1;
    }
    return 0;
}

static int test_no_platform_headers(void) {
    size_t i;
    for (i = 0; i < sizeof(g_ui_core_files) / sizeof(g_ui_core_files[0]); ++i) {
        FILE *file = fopen(g_ui_core_files[i], "rb");
        char line[1024];
        CHECK(file != NULL, "all reserved nk_ui headers and C files must exist");
        /* nk_ui_sdl_input.c is the SDL edge by design (it includes SDL3 behind
           NK_UI_WITH_SDL3); only its existence is checked here. The SDL-free
           headers it implements are still scanned. */
        if (strstr(g_ui_core_files[i], "nk_ui_sdl_input.c") != NULL) {
            fclose(file);
            continue;
        }
        while (fgets(line, sizeof(line), file) != NULL) {
            if (strstr(line, "#include") != NULL) {
                CHECK(strstr(line, "SDL") == NULL, "nk_ui core must not include SDL headers");
                CHECK(!contains_ascii_case_insensitive(line, "vulkan"),
                      "nk_ui core must not include Vulkan headers");
            }
        }
        fclose(file);
    }
    return 1;
}

static int test_theme_tokens(void) {
    const NkUiTheme *theme = nk_ui_theme_n1();
    CHECK(theme != NULL, "N1 theme must be available");
    CHECK(theme->accent_dark == 0xD7F24AFFu, "dark N1 accent token");
    CHECK(theme->accent_light == 0x6E9600FFu, "light N1 accent token");
    CHECK(theme->ready == 0x2DD4BFFFu, "Ready teal token");
    CHECK(theme->warning == 0xDBB024FFu, "warning gold token");
    CHECK(theme->ink == 0x10151CFFu && theme->paper == 0xF5F7FAFFu,
          "N1 ink and paper tokens");
    return 1;
}

static int test_scale_model(void) {
    NkUiViewport viewport = {640, 400, 6.0f, 0, 150};
    NkUiScale scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 4.0f, "automatic UI density clamps at four");
    CHECK(scale.logical_w == 160.0f && scale.logical_h == 100.0f,
          "logical layout uses the clamped density");
    viewport.display_scale = 0.5f;
    viewport.text_size_pct = 90;
    scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 1.0f, "UI density clamps at one");
    viewport.display_scale = 1.0f;
    viewport.ui_scale_pct = 200;
    viewport.text_size_pct = 150;
    scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 3.0f, "explicit UI and text scales multiply");
    return 1;
}

static int test_explicit_ui_scale_values(void) {
    static const int supported[] = {100, 125, 150, 200};
    NkUiViewport viewport = {1600, 1000, 2.0f, 0, 100};
    NkUiScale scale;
    size_t i;
    scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 2.0f, "ui_scale_pct 0 follows the display scale");
    viewport.ui_scale_pct = 100;
    scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 1.0f, "ui_scale_pct 100 forces 1x on a high-DPI display");
    CHECK(scale.logical_w == 1600.0f && scale.logical_h == 1000.0f,
          "forced 1x keeps logical size equal to pixel size");
    for (i = 0; i < sizeof(supported) / sizeof(supported[0]); ++i) {
        viewport.ui_scale_pct = supported[i];
        scale = nk_ui_scale_for_viewport(&viewport);
        CHECK(scale.density == (float)supported[i] / 100.0f,
              "each supported explicit ui_scale_pct sets that density");
    }
    viewport.ui_scale_pct = 175;
    scale = nk_ui_scale_for_viewport(&viewport);
    CHECK(scale.density == 1.0f, "an unsupported ui_scale_pct falls back to 1x");
    return 1;
}

static int test_typed_prefs_and_events(void) {
    NkUiPrefs prefs;
    NkUiEvent event;
    NkUi *ui;
    memset(&prefs, 0, sizeof(prefs));
    memset(&event, 0, sizeof(event));
    CHECK(prefs.prompt_style == NK_UI_PROMPT_NEUTRAL,
          "zeroed prefs select the neutral prompt set");
    CHECK(prefs.repeat == NK_UI_REPEAT_NORMAL, "zeroed prefs select normal repeat");
    CHECK(event.dir == NK_UI_DIR_NONE && event.pad_family == NK_UI_PAD_NEUTRAL,
          "zeroed events carry no direction and a neutral pad family");
    CHECK(NK_UI_PROMPT_NEUTRAL != NK_UI_PROMPT_LETTERS_A_SOUTH &&
          NK_UI_PROMPT_LETTERS_A_SOUTH != NK_UI_PROMPT_LETTERS_A_EAST &&
          NK_UI_PROMPT_LETTERS_A_EAST != NK_UI_PROMPT_KEYCAPS &&
          NK_UI_PROMPT_NEUTRAL != NK_UI_PROMPT_KEYCAPS,
          "prompt styles are distinct");
    CHECK(NK_UI_REPEAT_SLOW != NK_UI_REPEAT_NORMAL &&
          NK_UI_REPEAT_NORMAL != NK_UI_REPEAT_FAST &&
          NK_UI_REPEAT_SLOW != NK_UI_REPEAT_FAST, "repeat rates are distinct");
    prefs.prompt_style = NK_UI_PROMPT_KEYCAPS;
    prefs.repeat = NK_UI_REPEAT_FAST;
    event.type = NK_UI_EV_NAV;
    event.dir = NK_UI_DIR_LEFT;
    event.pad_family = NK_UI_PAD_LETTERS_A_EAST;
    ui = nk_ui_create(NULL);
    CHECK(ui != NULL, "create context for typed prefs");
    nk_ui_set_prefs(ui, &prefs);
    nk_ui_push_event(ui, &event);
    nk_ui_destroy(ui);
    return 1;
}

static int theme_probe_dump(const NkUiTheme *theme, char *dump, size_t cap) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiLayout layout;
    NkUiStyle style;
    NkUi *ui;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    config.theme = theme;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 160.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 80.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_COLUMN;
    memset(&style, 0, sizeof(style));
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create context with NULL or explicit N1 theme");
    nk_ui_begin(ui, 37.0);
    CHECK(nk_ui_begin_container(ui, 1u, &layout, &style), "open theme probe container");
    nk_ui_label(ui, 2u, "Theme", 0, 16);
    (void)nk_ui_button(ui, 3u, "Go", NK_UI_BUTTON_PRIMARY);
    nk_ui_end_container(ui);
    CHECK(nk_ui_end(ui) != NULL, "complete theme probe frame");
    CHECK(nk_ui_dump(ui, dump, cap) < cap, "theme probe dump fits");
    nk_ui_destroy(ui);
    return 1;
}

static int test_theme_access(void) {
    char with_null[8192];
    char with_n1[8192];
    CHECK(nk_ui_theme_n1() != NULL, "public nk_ui_theme_n1 returns a theme");
    CHECK(nk_ui_theme_n1() == nk_ui_theme_n1(), "nk_ui_theme_n1 returns one stable pointer");
    CHECK(theme_probe_dump(NULL, with_null, sizeof(with_null)), "dump with a NULL theme");
    CHECK(theme_probe_dump(nk_ui_theme_n1(), with_n1, sizeof(with_n1)),
          "dump with the explicit N1 theme");
    CHECK(strcmp(with_null, with_n1) == 0, "a NULL theme selects the N1 theme");
    return 1;
}

static int test_overlay_rejects_waves(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    double wake = 0.0;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create overlay context");
    nk_ui_begin(ui, 37.0);
    CHECK(!nk_ui_waves_element(ui, 9u, (NkUiRect){0.0f, 0.0f, 64.0f, 64.0f}, 0u),
          "overlay host returns false for waves instead of aborting");
    CHECK(nk_ui_end(ui) != NULL, "overlay frame still completes");
    CHECK(!nk_ui_next_wake(ui, &wake), "overlay frame schedules no wave wake");
    nk_ui_destroy(ui);
    return 1;
}

static int test_container_return_contract(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiLayout layout;
    NkUi *ui;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create container contract context");
    CHECK(!nk_ui_begin_container(ui, 1u, &layout, NULL),
          "begin_container returns false before nk_ui_begin");
    nk_ui_begin(ui, 37.0);
    CHECK(!nk_ui_begin_container(ui, 1u, NULL, NULL),
          "begin_container returns false for a NULL layout");
    CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL),
          "begin_container returns true when opened");
    nk_ui_end_container(ui);
    CHECK(nk_ui_end(ui) != NULL, "balanced containers leave a good frame");
    nk_ui_destroy(ui);
    return 1;
}

static int test_backend_declarations(void) {
    CHECK(!nk_ui_vk_render(NULL, NULL), "Vulkan render entry point refuses a NULL list");
    CHECK(!nk_ui_sdlr_render(NULL, NULL), "SDL renderer entry point refuses a NULL list");
    return 1;
}

/* Rect edges land on whole pixels; the quarter rings of rounded borders put
   their edges on pixel centres, so a vertex is on a whole or a half pixel. */
static int rect_vertices_are_integral(const NkUiDrawList *list) {
    uint32_t i;
    if (list == NULL) return 0;
    for (i = 0; i < list->nv; ++i) {
        if (!isfinite(list->v[i].x) || !isfinite(list->v[i].y) ||
            floorf(list->v[i].x * 2.0f) != list->v[i].x * 2.0f ||
            floorf(list->v[i].y * 2.0f) != list->v[i].y * 2.0f) {
            return 0;
        }
    }
    return 1;
}

static int append_text(char *out, size_t cap, size_t *used, const char *text) {
    int written;
    if (*used >= cap) return 0;
    written = snprintf(out + *used, cap - *used, "%s", text);
    if (written < 0 || (size_t)written >= cap - *used) return 0;
    *used += (size_t)written;
    return 1;
}

static int dump_synthetic_frame(const char *scale_name, int pixel_w, int pixel_h,
                                float display_scale, char *out, size_t cap,
                                size_t *used) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiViewport viewport;
    NkUiLayout root_layout;
    NkUiStyle root_style;
    NkUi *ui;
    const NkUiDrawList *list;
    char dump[8192];
    size_t dump_size;

    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create Clay-backed nk_ui context");
    viewport.pixel_w = pixel_w;
    viewport.pixel_h = pixel_h;
    viewport.display_scale = display_scale;
    viewport.ui_scale_pct = 0;
    viewport.text_size_pct = 100;
    nk_ui_set_viewport(ui, &viewport);
    nk_ui_begin(ui, 37.0);

    memset(&root_layout, 0, sizeof(root_layout));
    memset(&root_style, 0, sizeof(root_style));
    root_layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    root_layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    root_layout.direction = NK_UI_LAYOUT_COLUMN;
    root_layout.gap = 10.0f;
    root_layout.padding_left = 16.0f;
    root_layout.padding_top = 16.0f;
    root_layout.padding_right = 16.0f;
    root_layout.padding_bottom = 16.0f;
    root_style.fill_rgba = 0x0B0F14FFu;
    root_style.border_rgba = 0x18202BFFu;
    root_style.border_width = 1.0f;
    root_style.corner_radius = 8.0f;
    CHECK(nk_ui_begin_container(ui, 1u, &root_layout, &root_style),
          "open synthetic root container");
    nk_ui_label(ui, 2u, "Core layout", 0, 16);
    (void)nk_ui_button(ui, 3u, "Open library", NK_UI_BUTTON_PRIMARY);
    (void)nk_ui_button(ui, 4u, "Settings", 0u);
    nk_ui_end_container(ui);
    list = nk_ui_end(ui);
    CHECK(list != NULL, "complete Clay frame");
    CHECK(rect_vertices_are_integral(list), "all rect-class vertices land on whole pixels");
    dump_size = nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(dump_size < sizeof(dump), "synthetic dump fits test buffer");
    CHECK(append_text(out, cap, used, scale_name), "append scale marker");
    CHECK(append_text(out, cap, used, dump), "append synthetic core dump");
    nk_ui_destroy(ui);
    return 1;
}

static int build_golden_output(char *out, size_t cap) {
    size_t used = 0u;
    out[0] = '\0';
    CHECK(dump_synthetic_frame("scale=1.00\n", 640, 400, 1.0f,
                               out, cap, &used), "build 1x golden");
    CHECK(dump_synthetic_frame("scale=1.25\n", 800, 500, 1.25f,
                               out, cap, &used), "build 1.25x golden");
    CHECK(dump_synthetic_frame("scale=1.50\n", 960, 600, 1.5f,
                               out, cap, &used), "build 1.5x golden");
    CHECK(dump_synthetic_frame("scale=2.00\n", 1280, 800, 2.0f,
                               out, cap, &used), "build 2x golden");
    return 1;
}

static float srgb_linear(float value) {
    if (value <= 0.04045f) return value / 12.92f;
    return powf((value + 0.055f) / 1.055f, 2.4f);
}

static float luminance(float r, float g, float b) {
    return 0.2126f * srgb_linear(r) + 0.7152f * srgb_linear(g) +
           0.0722f * srgb_linear(b);
}

static int test_wave_caps(void) {
    const uint32_t surface = 0x0B0F14FFu;
    float surface_r = 11.0f / 255.0f;
    float surface_g = 15.0f / 255.0f;
    float surface_b = 20.0f / 255.0f;
    unsigned tint;
    uint8_t *pixels = (uint8_t *)malloc(160u * 96u * 4u);
    CHECK(pixels != NULL, "allocate wave reference buffer");
    for (tint = 0; tint <= NK_UI_WAVES_TINT_COUNT; ++tint) {
        NkUiWavePalette palette;
        uint32_t x;
        uint32_t y;
        CHECK(nk_ui_waves_palette(tint, &palette), "load base or monthly wave palette");
        CHECK(palette.cap > 0.0f && palette.cap <= NK_UI_WAVES_ALPHA_CAP_MAX + 1e-6f,
              "wave alpha cap remains at or under 0.24");
        CHECK(fabsf(palette.cap - nk_ui_waves_compute_cap(&palette, surface)) < 1e-6f,
              "wave cap is derived from its exact palette");
        CHECK(nk_ui_waves_contrast_ok(&palette, surface, NK_UI_WAVES_LUMINANCE_MAX),
              "wave palette stays below the contrast luminance limit");
        CHECK(nk_ui_waves_render_rgba(&palette, 37.0, 160u, 96u,
                                      pixels, 160u * 4u),
              "render project-authored CPU wave reference");
        for (y = 0; y < 96u; ++y) {
            for (x = 0; x < 160u; ++x) {
                const uint8_t *pixel = pixels + ((size_t)y * 160u + x) * 4u;
                float alpha = (float)pixel[3] / 255.0f;
                float r = (float)pixel[0] / 255.0f + surface_r * (1.0f - alpha);
                float g = (float)pixel[1] / 255.0f + surface_g * (1.0f - alpha);
                float b = (float)pixel[2] / 255.0f + surface_b * (1.0f - alpha);
                CHECK(alpha <= palette.cap + 1.0f / 255.0f,
                      "rendered alpha respects the palette cap");
                CHECK(luminance(r, g, b) <= NK_UI_WAVES_LUMINANCE_MAX + 0.0015f,
                      "rendered wave pixels preserve the contrast budget");
            }
        }
    }
    free(pixels);
    return 1;
}

static int test_rounding_edges(void) {
    NkUiPixelRect first = nk_ui_round_rect((NkUiRect){0.2f, 0.3f, 17.1f, 8.2f}, 1.25f);
    NkUiPixelRect neighbor = nk_ui_round_rect((NkUiRect){17.3f, 0.3f, 11.0f, 8.2f}, 1.25f);
    CHECK(first.x1 == neighbor.x0, "adjacent logical edges round to the same device pixel");
    CHECK(first.x0 == 0 && first.y0 == 0 && first.x1 == 22 && first.y1 == 11 &&
          neighbor.x0 == 22,
          "pixel rectangle edges use nearest whole device pixels");
    return 1;
}

static int test_focused_button_activation(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiEvent event;
    NkUi *ui;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create button activation context");
    nk_ui_focus(ui, 42u);
    memset(&event, 0, sizeof(event));
    event.type = NK_UI_EV_ACTIVATE;
    event.down = true;
    nk_ui_push_event(ui, &event);
    nk_ui_begin(ui, 37.0);
    CHECK(nk_ui_button(ui, 42u, "Activate", 0u),
          "focused button reports a queued activation");
    CHECK(nk_ui_end(ui) != NULL, "complete focused button frame");
    nk_ui_destroy(ui);
    return 1;
}

static int test_golden(const char *actual, size_t actual_size) {
    FILE *file = fopen("fixtures/nk_ui/core_synthetic.golden", "rb");
    char expected[32768];
    size_t read_size;
    CHECK(file != NULL, "open checked-in core golden");
    read_size = fread(expected, 1u, sizeof(expected) - 1u, file);
    fclose(file);
    expected[read_size] = '\0';
    CHECK(read_size == actual_size, "core golden byte count matches all four scales");
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "FAIL: core golden differs\n--- actual ---\n%s--- expected ---\n%s",
                actual, expected);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    char actual[32768];
    size_t actual_size;
    if (!test_no_platform_headers() || !test_theme_tokens() || !test_scale_model() ||
        !test_explicit_ui_scale_values() || !test_typed_prefs_and_events() ||
        !test_theme_access() || !test_overlay_rejects_waves() ||
        !test_container_return_contract() || !test_backend_declarations() ||
        !test_rounding_edges() || !test_focused_button_activation() || !test_wave_caps()) {
        return 1;
    }
    if (!build_golden_output(actual, sizeof(actual))) {
        return 1;
    }
    actual_size = strlen(actual);
    if (argc > 1 && strcmp(argv[1], "--print") == 0) {
        fputs(actual, stdout);
        return 0;
    }
    if (!test_golden(actual, actual_size)) {
        return 1;
    }
    puts("nk_ui core goldens, pixel rounding, SDL/Vulkan boundary and wave caps passed");
    return 0;
}
