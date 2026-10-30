/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "nk_ui.h"
#include "nk_ui_assets.h"
#include "nk_ui_draw.h"
#include "nk_ui_image.h"
#include "nk_ui_text.h"
#include "nk_ui_theme.h"
#include "nk_ui_waves.h"
#include "nk_ui_widgets.h"
#include "backend/nk_ui_sdlr.h"
#include "backend/nk_ui_vk.h"
#include "clay.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

size_t nk_ui_dump_draw_list(const NkUiDrawList *list, char *out, size_t cap);
bool nk_ui_clay_error_kind(int type, const char **kind);

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
    "src/ui/nk_ui_image.h",
    "src/ui/nk_ui_image.c",
    "src/ui/nk_ui_assets.h",
    "src/ui/nk_ui_assets.c",
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
    NkUiViewport viewport = {640, 400, 6.0f, 0, 150, 0.0f, 0.0f};
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
    NkUiViewport viewport = {1600, 1000, 2.0f, 0, 100, 0.0f, 0.0f};
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
    /* Every field, min_logical_w and min_logical_h included: a stale minimum
       layout lowers the density. */
    memset(&viewport, 0, sizeof(viewport));
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

static int test_progress_sweep(void) {
    const float travel = 300.0f;
    float previous = nk_ui_progress_sweep_offset(travel, 0.0);
    int step;
    CHECK(previous == 0.0f, "a zero phase holds the sweep at the start");
    CHECK(fabsf(nk_ui_progress_sweep_offset(travel, 1.2) - travel) < 0.5f,
          "the sweep reaches the far end half way through its cycle");
    CHECK(nk_ui_progress_sweep_offset(0.0f, 1.0) == 0.0f, "no travel, no movement");
    /* Three cycles at 120 Hz: the segment never jumps back to the start. */
    for (step = 1; step <= 3 * 288; ++step) {
        float offset = nk_ui_progress_sweep_offset(travel, step / 120.0);
        CHECK(offset >= 0.0f && offset <= travel, "the sweep stays on the track");
        CHECK(fabsf(offset - previous) < 0.02f * travel, "the sweep moves without a jump");
        previous = offset;
    }
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

/* One frame of a row of two buttons, base and base + 1. Returns the activated
   button, or 0. */
/* The clock draw_button_pair draws at; the tests of motion move it. */
static double g_button_pair_now = 37.0;

static NkUiId draw_button_pair(NkUi *ui, NkUiId base, NkUiId focus_request) {
    NkUiLayout layout;
    NkUiStyle style;
    NkUiId activated = 0u;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 320.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 60.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_ROW;
    layout.gap = 16.0f;
    memset(&style, 0, sizeof(style));
    nk_ui_begin(ui, g_button_pair_now);
    if (focus_request != 0u) {
        nk_ui_focus(ui, focus_request);
    }
    if (nk_ui_begin_container(ui, base + 100u, &layout, &style)) {
        if (nk_ui_button(ui, base, "Keep", NK_UI_BUTTON_PRIMARY)) activated = base;
        if (nk_ui_button(ui, base + 1u, "Remove", 0u)) activated = base + 1u;
        nk_ui_end_container(ui);
    }
    (void)nk_ui_end(ui);
    return activated;
}

static int test_input_waits_for_a_new_screen_layout(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiEvent nav;
    NkUiEvent activate;
    NkUi *ui;
    NkUiId activated = 0u;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    memset(&nav, 0, sizeof(nav));
    nav.type = NK_UI_EV_NAV;
    nav.dir = NK_UI_DIR_RIGHT;
    nav.down = true;
    memset(&activate, 0, sizeof(activate));
    activate.type = NK_UI_EV_ACTIVATE;
    activate.down = true;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create new screen context");
    for (frame = 0; frame < 3; ++frame) {
        (void)draw_button_pair(ui, 10u, frame == 0 ? 10u : 0u);
    }
    /* A new screen opens on its first button; Right then Enter arrive right after its
       first frame, before Clay has laid out its buttons. */
    (void)draw_button_pair(ui, 20u, 20u);
    CHECK(nk_ui_focused(ui) == 20u, "the new screen opens on its requested button");
    nk_ui_push_event(ui, &nav);
    nk_ui_push_event(ui, &activate);
    for (frame = 0; frame < 3 && activated == 0u; ++frame) {
        activated = draw_button_pair(ui, 20u, 0u);
    }
    CHECK(activated == 21u, "Right then Enter on a new screen's first frames picks the second button");
    CHECK(nk_ui_focused(ui) == 21u, "the move is kept, not lost to the unplaced graph");
    nk_ui_destroy(ui);
    return 1;
}

static void push_pointer(NkUi *ui, float x, float y, bool down) {
    NkUiEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NK_UI_EV_POINTER;
    event.x = x;
    event.y = y;
    event.down = down;
    nk_ui_push_event(ui, &event);
}

/* The owner's first run: one click on COPY FILES AND CONTINUE swapped in the
   copy screen, whose CANCEL sat where the button was, and the same click
   cancelled the copy. A click activates a control only when its press and its
   release both land on it; a held pointer that moves is a drag, not a press. */
static int test_click_needs_press_and_release_on_one_control(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    NkUiId activated = 0u;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create click context");
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, frame == 0 ? 10u : 0u);

    /* Press and release on one button: it fires on the release, once. */
    push_pointer(ui, 10.0f, 10.0f, true);
    CHECK(draw_button_pair(ui, 10u, 0u) == 0u, "a press alone activates nothing");
    CHECK(nk_ui_focused(ui) == 10u, "a press focuses the control under it");
    push_pointer(ui, 10.0f, 10.0f, false);
    CHECK(draw_button_pair(ui, 10u, 0u) == 10u, "the release on the pressed button fires it");
    CHECK(draw_button_pair(ui, 10u, 0u) == 0u, "it fires once");

    /* The copy button (10) and the copy screen's CANCEL (20), which takes its
       place once 10 fires. One click, one event a frame: the press, two small
       moves with the button held, the release. Before, the press fired 10 and
       a held move then pressed 20. */
    {
        bool copying = false;
        bool cancelled = false;
        int fired_on = -1;
        for (frame = 0; frame < 8; ++frame) {
            if (frame == 0) push_pointer(ui, 10.0f, 10.0f, true);
            if (frame == 1) push_pointer(ui, 11.0f, 10.0f, true);
            if (frame == 2) push_pointer(ui, 12.0f, 11.0f, true);
            if (frame == 3) push_pointer(ui, 12.0f, 11.0f, false);
            activated = draw_button_pair(ui, copying ? 20u : 10u, 0u);
            if (activated == 10u && !copying) {
                copying = true;
                fired_on = frame;
            } else if (activated == 20u) {
                cancelled = true;
            }
        }
        CHECK(copying && fired_on == 3, "the copy button fires once, on the release");
        CHECK(!cancelled, "the click does not press the CANCEL that took its place");
    }

    /* A host that changes screen drops the press, even for a control with the
       same id in the same place. */
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    push_pointer(ui, 10.0f, 10.0f, true);
    (void)draw_button_pair(ui, 10u, 0u);
    nk_ui_cancel_press(ui);
    push_pointer(ui, 10.0f, 10.0f, false);
    activated = 0u;
    for (frame = 0; frame < 3 && activated == 0u; ++frame) {
        activated = draw_button_pair(ui, 10u, 0u);
    }
    CHECK(activated == 0u, "a cancelled press's release activates nothing");

    /* A press on one button released on the other activates neither. */
    push_pointer(ui, 10.0f, 10.0f, true);
    (void)draw_button_pair(ui, 10u, 0u);
    push_pointer(ui, 300.0f, 10.0f, false);
    activated = 0u;
    for (frame = 0; frame < 3 && activated == 0u; ++frame) {
        activated = draw_button_pair(ui, 10u, 0u);
    }
    CHECK(activated == 0u, "a release away from the pressed button activates nothing");
    nk_ui_destroy(ui);
    return 1;
}

/* A row: a control that owns left and right (a segmented control), then a plain
   button to its right. */
static void horizontal_row_frame(NkUi *ui, NkUiNavDir *taken, bool *took) {
    NkUiLayout layout;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_ROW;
    layout.gap = 12.0f;
    nk_ui_begin(ui, 37.0);
    *took = nk_ui_take_event(ui, NK_UI_EV_NAV, taken);
    if (nk_ui_begin_container(ui, 1u, &layout, NULL)) {
        (void)nk_ui_button(ui, 41u, "Segmented", NK_UI_BUTTON_OWNS_HORIZONTAL);
        (void)nk_ui_button(ui, 43u, "Plain", 0u);
        nk_ui_end_container(ui);
    }
    (void)nk_ui_end(ui);
}

static void push_nav(NkUi *ui, NkUiNavDir dir) {
    NkUiEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NK_UI_EV_NAV;
    event.dir = dir;
    event.down = true;
    nk_ui_push_event(ui, &event);
}

/* True when the last frame's draw list fills a rect for `id` with `rgba`. */
static bool drew_fill(NkUi *ui, NkUiId id, uint32_t rgba) {
    char dump[8192];
    char want[48];
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    (void)snprintf(want, sizeof(want), "%08X RECT", (unsigned)id);
    {
        const char *line = strstr(dump, want);
        char colour[16];
        (void)snprintf(colour, sizeof(colour), " %08X ", (unsigned)rgba);
        while (line != NULL) {
            const char *end = strchr(line, '\n');
            size_t length = end != NULL ? (size_t)(end - line) : strlen(line);
            const char *found = strstr(line, colour);
            if (found != NULL && (size_t)(found - line) < length) return true;
            line = strstr(line + 1, want);
        }
    }
    return false;
}

/* A 640 x 400 window at 1x, so nothing in these frames is culled. */
static void set_test_window(NkUi *ui) {
    NkUiViewport viewport;
    memset(&viewport, 0, sizeof(viewport));
    viewport.pixel_w = 640;
    viewport.pixel_h = 400;
    viewport.display_scale = 1.0f;
    viewport.text_size_pct = 100;
    nk_ui_set_viewport(ui, &viewport);
}

static void push_leave(NkUi *ui) {
    NkUiEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NK_UI_EV_POINTER_LEAVE;
    nk_ui_push_event(ui, &event);
}

/* Hover and pressed looks come from Clay's pointer state over the last layout.
   Hover is for the pointer only: a key hides it until the pointer moves, it
   clears when the pointer leaves the window, and it never moves focus. The
   looks are checked at a clock that does not move, so motion is cut here;
   test_hover_fill_eases covers the easing. */
static int test_pointer_hover_and_press(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create hover context");
    set_test_window(ui);
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, frame == 0 ? 10u : 0u);
    CHECK(!nk_ui_hovered(ui, 10u) && !nk_ui_hovered(ui, 11u), "nothing is hovered before the pointer moves");

    push_pointer(ui, 100.0f, 10.0f, false);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_hovered(ui, 11u) && !nk_ui_hovered(ui, 10u), "the pointer hovers the button under it");
    CHECK(nk_ui_focused(ui) == 10u, "hover does not move focus");
    CHECK(drew_fill(ui, 11u, theme->surface_high), "a hovered secondary button fills in");

    push_nav(ui, NK_UI_DIR_RIGHT);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(!nk_ui_hovered(ui, 11u), "a key hides hover");
    push_pointer(ui, 101.0f, 10.0f, false);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_hovered(ui, 11u), "moving the pointer shows it again");

    push_pointer(ui, 101.0f, 10.0f, true);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_pressed(ui, 11u) && drew_fill(ui, 11u, theme->pressed),
          "a button held under the pointer is drawn pressed");
    push_pointer(ui, 10.0f, 10.0f, true);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(!nk_ui_pressed(ui, 11u) && !nk_ui_pressed(ui, 10u),
          "a held pointer dragged off it presses nothing");
    push_pointer(ui, 10.0f, 10.0f, false);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_hovered(ui, 10u) && drew_fill(ui, 10u, theme->accent_hover),
          "a hovered primary button lightens");

    push_leave(ui);
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(!nk_ui_hovered(ui, 10u) && drew_fill(ui, 10u, theme->accent_dark),
          "leaving the window clears hover");
    nk_ui_destroy(ui);
    return 1;
}

/* A button's hover fill eases in over NK_UI_MOTION_FAST: half way through it
   is neither the resting fill nor the hover fill, and nk_ui says it is still
   moving; then it is the hover fill and still. Reduce motion cuts it. */
static int test_hover_fill_eases(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create hover easing context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) {
        g_button_pair_now = 20.0 + frame * 0.01;
        (void)draw_button_pair(ui, 10u, frame == 0 ? 10u : 0u);
    }
    CHECK(!drew_fill(ui, 11u, theme->surface_high) && !nk_ui_in_motion(ui),
          "the secondary button rests unfilled");
    push_pointer(ui, 100.0f, 10.0f, false);
    g_button_pair_now = 20.05;
    (void)draw_button_pair(ui, 10u, 0u);
    g_button_pair_now = 20.05 + NK_UI_MOTION_FAST / 2.0;
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_hovered(ui, 11u) && !drew_fill(ui, 11u, theme->surface_high) &&
              nk_ui_in_motion(ui),
          "half way through, the hover fill is still easing in");
    for (frame = 1; frame <= 3; ++frame) {
        g_button_pair_now = 20.05 + NK_UI_MOTION_FAST * 2.0 * frame;
        (void)draw_button_pair(ui, 10u, 0u);
    }
    CHECK(drew_fill(ui, 11u, theme->surface_high) && !nk_ui_in_motion(ui),
          "then the button is filled and still");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    push_pointer(ui, 10.0f, 10.0f, false);
    g_button_pair_now = 21.0;
    (void)draw_button_pair(ui, 10u, 0u);
    CHECK(drew_fill(ui, 10u, theme->accent_hover) && !nk_ui_in_motion(ui),
          "Reduce motion: the hover fill is there at once");
    g_button_pair_now = 37.0;
    nk_ui_destroy(ui);
    return 1;
}

/* Three 44 px buttons in a 50 px clip: the pointer below the clip is over no
   button, though the second and third run on under it. */
static NkUiId draw_clipped_column(NkUi *ui) {
    NkUiLayout layout;
    NkUiId activated = 0u;
    NkUiId id;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 200.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 50.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_COLUMN;
    layout.scroll = true;
    nk_ui_begin(ui, 37.0);
    if (nk_ui_begin_container(ui, 1u, &layout, NULL)) {
        for (id = 20u; id < 23u; ++id) {
            if (nk_ui_button(ui, id, "Row", NK_UI_BUTTON_GROW)) activated = id;
        }
        nk_ui_end_container(ui);
    }
    (void)nk_ui_end(ui);
    return activated;
}

static int test_pointer_respects_clip(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    NkUiId activated = 0u;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create clip context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_clipped_column(ui);
    CHECK(nk_ui_focused(ui) == 20u, "the first row starts focused");
    push_pointer(ui, 50.0f, 70.0f, false);
    (void)draw_clipped_column(ui);
    CHECK(!nk_ui_hovered(ui, 21u), "a row clipped away is not hovered");
    push_pointer(ui, 50.0f, 70.0f, true);
    (void)draw_clipped_column(ui);
    push_pointer(ui, 50.0f, 70.0f, false);
    for (frame = 0; frame < 2 && activated == 0u; ++frame) activated = draw_clipped_column(ui);
    CHECK(activated == 0u && nk_ui_focused(ui) == 20u, "nor clicked");
    push_pointer(ui, 50.0f, 47.0f, false);
    (void)draw_clipped_column(ui);
    CHECK(nk_ui_hovered(ui, 21u), "the part inside the clip is");
    nk_ui_destroy(ui);
    return 1;
}

/* The top of text run `id` in the last frame's draw list, in device px, or
   -1 when the frame drew no such run. */
static int drawn_text_top(NkUi *ui, NkUiId id) {
    char dump[16384];
    char want[32];
    const char *line;
    int x0 = 0;
    int y0 = -1;
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    (void)snprintf(want, sizeof(want), "%08X TEXT ", (unsigned)id);
    line = strstr(dump, want);
    if (line == NULL || sscanf(line + strlen(want), "%d %d", &x0, &y0) != 2) return -1;
    return y0;
}

/* The draw list of the last scroll-list frame. */
static const NkUiDrawList *g_scroll_list;

/* Ten 44 px rows (ids 40 to 49) in a 100 px scroll container (id 30) at the
   window's top left. Returns the row activated this frame, or 0. */
static NkUiId draw_scroll_list(NkUi *ui, bool *wheel_left) {
    NkUiLayout root;
    NkUiLayout list;
    NkUiNavDir dir;
    NkUiId activated = 0u;
    NkUiId id;
    memset(&root, 0, sizeof(root));
    root.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    root.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    root.direction = NK_UI_LAYOUT_COLUMN;
    memset(&list, 0, sizeof(list));
    list.width = (NkUiSizing){NK_UI_SIZE_FIXED, 200.0f, 0.0f, 0.0f};
    list.height = (NkUiSizing){NK_UI_SIZE_FIXED, 100.0f, 0.0f, 0.0f};
    list.direction = NK_UI_LAYOUT_COLUMN;
    list.scroll = true;
    nk_ui_begin(ui, 37.0);
    if (wheel_left != NULL) *wheel_left = nk_ui_take_event(ui, NK_UI_EV_WHEEL, &dir);
    if (nk_ui_begin_container(ui, 1u, &root, NULL)) {
        if (nk_ui_begin_container(ui, 30u, &list, NULL)) {
            for (id = 40u; id < 50u; ++id) {
                if (nk_ui_button(ui, id, "Row", NK_UI_BUTTON_GROW)) activated = id;
            }
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    g_scroll_list = nk_ui_end(ui);
    return activated;
}

static void push_wheel(NkUi *ui, float x, float y, NkUiNavDir dir) {
    NkUiEvent event;
    memset(&event, 0, sizeof(event));
    event.type = NK_UI_EV_WHEEL;
    event.x = x;
    event.y = y;
    event.dir = dir;
    event.down = true;
    nk_ui_push_event(ui, &event);
}

static float scroll_offset(NkUi *ui) {
    NkUiScrollState state;
    return nk_ui_scroll_state(ui, 30u, &state) ? state.offset : -1.0f;
}

/* A scroll container scrolls through Clay: the wheel over it (a notch is 48
   px; a wheel anywhere else stays for the screen), a drag with the pointer
   held (and that drag is no click), focus moving to a row out of view, and
   nk_ui_scroll_to, which also sets where a container starts. */
static int test_scroll_container(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiScrollState state;
    NkUi *ui;
    NkUiId activated = 0u;
    bool wheel_left = false;
    int frame;
    int top;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create scroll context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_scroll_list(ui, NULL);
    CHECK(nk_ui_scroll_state(ui, 30u, &state) && state.offset == 0.0f &&
          state.max_offset == 340.0f && state.viewport == 100.0f,
          "the list starts at its top, with 340 px more than its 100 px of room");
    top = drawn_text_top(ui, 45u);

    push_wheel(ui, 50.0f, 50.0f, NK_UI_DIR_DOWN);
    (void)draw_scroll_list(ui, &wheel_left);
    CHECK(!wheel_left && scroll_offset(ui) == 48.0f && nk_ui_scroll_in_motion(ui),
          "a wheel notch over the list scrolls it 48 px and is not left for the screen");
    CHECK(drawn_text_top(ui, 45u) == top - 48, "the rows are drawn 48 px higher");
    (void)draw_scroll_list(ui, NULL);
    CHECK(scroll_offset(ui) == 48.0f && !nk_ui_scroll_in_motion(ui), "and stay there");
    push_wheel(ui, 400.0f, 300.0f, NK_UI_DIR_DOWN);
    (void)draw_scroll_list(ui, &wheel_left);
    CHECK(wheel_left && scroll_offset(ui) == 48.0f, "a wheel elsewhere stays for the screen");

    /* Keys: down from row 40 to row 47 (308 to 352 px down the content). */
    nk_ui_scroll_to(ui, 30u, 0.0f);
    (void)draw_scroll_list(ui, NULL);
    push_nav(ui, NK_UI_DIR_DOWN);
    (void)draw_scroll_list(ui, NULL);
    CHECK(nk_ui_focused(ui) == 41u && scroll_offset(ui) == 0.0f,
          "a move judges rows where the list was last drawn, so row 41 needs no scroll");
    for (frame = 0; frame < 6; ++frame) {
        push_nav(ui, NK_UI_DIR_DOWN);
        (void)draw_scroll_list(ui, NULL);
    }
    CHECK(nk_ui_focused(ui) == 47u, "focus walks down to row 47");
    CHECK(scroll_offset(ui) == 252.0f, "the list scrolls just enough to show the focused row");
    push_nav(ui, NK_UI_DIR_UP);
    (void)draw_scroll_list(ui, NULL);
    CHECK(scroll_offset(ui) == 252.0f, "a row already in view needs no scroll");

    /* A drag: held on row 47 and moved up 70 px, the list follows the pointer
       and the release on the same row is no click. */
    push_pointer(ui, 50.0f, 80.0f, true);
    (void)draw_scroll_list(ui, NULL);
    push_pointer(ui, 50.0f, 10.0f, true);
    activated = draw_scroll_list(ui, NULL);
    CHECK(scroll_offset(ui) == 322.0f, "a drag scrolls the list with the pointer");
    push_pointer(ui, 50.0f, 10.0f, false);
    for (frame = 0; frame < 2 && activated == 0u; ++frame) activated = draw_scroll_list(ui, NULL);
    CHECK(activated == 0u, "a drag that scrolled is no click");

    nk_ui_scroll_to(ui, 30u, 1000.0f);
    (void)draw_scroll_list(ui, NULL);
    CHECK(scroll_offset(ui) == 340.0f, "nk_ui_scroll_to stops at the end of the content");
    nk_ui_destroy(ui);

    /* A list asked to start 200 px down is drawn there from its first frame. */
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create second scroll context");
    set_test_window(ui);
    nk_ui_scroll_to(ui, 30u, 200.0f);
    (void)draw_scroll_list(ui, NULL);
    CHECK(drawn_text_top(ui, 45u) == top - 200 && scroll_offset(ui) == 200.0f,
          "a container starts where nk_ui_scroll_to asked");
    nk_ui_destroy(ui);
    return 1;
}

/* A 60 px band (id 1) holding one wide page button (60); with `modal`, a
   200 px dialog card (70) with its own button (71) floats centred over it. */
static NkUiId draw_modal_page(NkUi *ui, bool modal) {
    NkUiLayout band;
    NkUiId activated = 0u;
    memset(&band, 0, sizeof(band));
    band.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    band.height = (NkUiSizing){NK_UI_SIZE_FIXED, 60.0f, 0.0f, 0.0f};
    band.direction = NK_UI_LAYOUT_COLUMN;
    nk_ui_begin(ui, 37.0);
    if (nk_ui_begin_container(ui, 1u, &band, NULL)) {
        if (nk_ui_button(ui, 60u, "Page", NK_UI_BUTTON_GROW)) activated = 60u;
        if (modal && nk_ui_dialog_begin(ui, 70u, 200.0f)) {
            if (nk_ui_button(ui, 71u, "Card", 0u)) activated = 71u;
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    (void)nk_ui_end(ui);
    return activated;
}

/* A dialog card is a Clay floating element: it moves none of the page around
   it, draws over it, and takes the pointer, so the page under it is neither
   hovered nor clicked. */
static int test_modal_card_floats(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    NkUiId activated = 0u;
    int frame;
    int page_top;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create modal context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_modal_page(ui, false);
    page_top = drawn_text_top(ui, 60u);
    for (frame = 0; frame < 3; ++frame) (void)draw_modal_page(ui, true);
    CHECK(drawn_text_top(ui, 60u) == page_top, "the card moves nothing on the page");
    /* The card is 200 px wide, centred on the 640 px band, so its edge runs
       from 220 to 248 px before its button; the page button runs under it. */
    push_pointer(ui, 230.0f, 20.0f, false);
    (void)draw_modal_page(ui, true);
    CHECK(!nk_ui_hovered(ui, 60u) && !nk_ui_hovered(ui, 71u),
          "the pointer on the card's edge hovers nothing under it");
    push_pointer(ui, 230.0f, 20.0f, true);
    (void)draw_modal_page(ui, true);
    push_pointer(ui, 230.0f, 20.0f, false);
    for (frame = 0; frame < 2 && activated == 0u; ++frame) activated = draw_modal_page(ui, true);
    CHECK(activated == 0u, "nor clicks it");
    push_pointer(ui, 600.0f, 20.0f, false);
    (void)draw_modal_page(ui, true);
    CHECK(nk_ui_hovered(ui, 60u), "the page beside the card is still the pointer's");
    nk_ui_destroy(ui);
    return 1;
}

/* The focus ring is a floating element on its control: drawn after every
   other command, and clipped by the scroll container the control is in. */
static int test_focus_ring_floats(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiConfig config;
    const NkUiDrawList *list;
    NkUi *ui;
    uint32_t first_ring = UINT32_MAX;
    uint32_t last_other = 0u;
    uint32_t i;
    bool reaches_above = false;
    bool clipped = true;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create ring context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_scroll_list(ui, NULL);
    nk_ui_focus(ui, 41u);
    (void)draw_scroll_list(ui, NULL);
    /* Row 41 runs from 44 to 88 px down the list; 60 px down, it starts 16 px
       above the list's top and its ring 22 px above. */
    nk_ui_scroll_to(ui, 30u, 60.0f);
    (void)draw_scroll_list(ui, NULL);
    CHECK(nk_ui_focused(ui) == 41u, "row 41 keeps focus");
    list = g_scroll_list;
    CHECK(list != NULL, "the frame was drawn");
    for (i = 0u; i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_BORDER && cmd->element_id == 41u &&
            cmd->rgba_pm == theme->focus) {
            if (first_ring == UINT32_MAX) first_ring = i;
            if (cmd->bounds[1] < 0) reaches_above = true;
            if (cmd->clip[3] > cmd->clip[1] && cmd->clip[1] < 0) clipped = false;
        } else if (cmd->kind != NK_UI_DRAW_SCISSOR_START && cmd->kind != NK_UI_DRAW_SCISSOR_END) {
            last_other = i;
        }
    }
    CHECK(first_ring != UINT32_MAX && first_ring > last_other, "the ring is drawn over everything");
    CHECK(reaches_above && clipped, "the ring is clipped where the list clips its row");
    nk_ui_destroy(ui);
    return 1;
}

/* Two 120 px buttons, 200 px apart, drawn at `now_s`. Returns the left edge of
   the focus ring's band, or INT_MIN when no ring is drawn. */
static int draw_ring_pair(NkUi *ui, double now_s) {
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiLayout row;
    const NkUiDrawList *list;
    uint32_t i;
    memset(&row, 0, sizeof(row));
    row.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    row.height = (NkUiSizing){NK_UI_SIZE_FIT, 0.0f, 0.0f, 0.0f};
    row.direction = NK_UI_LAYOUT_ROW;
    row.gap = 80.0f;
    row.padding_left = 40.0f;
    row.padding_top = 40.0f;
    nk_ui_begin(ui, now_s);
    if (nk_ui_begin_container(ui, 90u, &row, NULL)) {
        NkUiLayout cell = row;
        cell.width = (NkUiSizing){NK_UI_SIZE_FIXED, 120.0f, 0.0f, 0.0f};
        cell.gap = 0.0f;
        cell.padding_left = 0.0f;
        cell.padding_top = 0.0f;
        if (nk_ui_begin_container(ui, 91u, &cell, NULL)) {
            (void)nk_ui_button(ui, 81u, "One", 0u);
            nk_ui_end_container(ui);
        }
        if (nk_ui_begin_container(ui, 92u, &cell, NULL)) {
            (void)nk_ui_button(ui, 82u, "Two", 0u);
            nk_ui_end_container(ui);
        }
        nk_ui_end_container(ui);
    }
    list = nk_ui_end(ui);
    for (i = 0u; list != NULL && i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_BORDER && cmd->rgba_pm == theme->focus) return cmd->bounds[0];
    }
    return INT_MIN;
}

/* The ring eases from the control focus leaves to the one it reaches, over
   NK_UI_MOTION_FAST, and nk_ui says so while it moves, so the host keeps
   drawing. Reduce motion makes the move a cut. */
static int test_focus_ring_moves(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int start;
    int mid;
    int end;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create ring move context");
    set_test_window(ui);
    nk_ui_focus(ui, 81u);
    for (frame = 0; frame < 3; ++frame) start = draw_ring_pair(ui, 10.0 + frame * 0.01);
    CHECK(start != INT_MIN && !nk_ui_in_motion(ui), "the ring rests on the first button");
    nk_ui_focus(ui, 82u);
    (void)draw_ring_pair(ui, 10.03);
    mid = draw_ring_pair(ui, 10.03 + NK_UI_MOTION_FAST / 2.0);
    CHECK(mid > start && mid < start + 200 && nk_ui_in_motion(ui),
          "half way through, the ring is between the buttons and still moving");
    for (frame = 1; frame <= 3; ++frame) end = draw_ring_pair(ui, 10.03 + NK_UI_MOTION_FAST * 2.0 * frame);
    CHECK(end == start + 200 && !nk_ui_in_motion(ui), "then it rests on the second button");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    nk_ui_focus(ui, 81u);
    (void)draw_ring_pair(ui, 11.0);
    CHECK(draw_ring_pair(ui, 11.0 + NK_UI_MOTION_FAST / 2.0) == start && !nk_ui_in_motion(ui),
          "Reduce motion: the ring is on the new control at once");
    nk_ui_destroy(ui);
    return 1;
}

/* A top bar whose second or third tab is active, drawn at `now_s`. Returns the
   left edge of the accent rule under the active tab, or INT_MIN. */
static int draw_tab_bar(NkUi *ui, double now_s, int active) {
    const NkUiTheme *theme = nk_ui_theme_n1();
    const NkUiSection sections[] = {
        {"Library", false, 0u}, {"Settings", active == 1, 0u}, {"Controller", active == 2, 0u}
    };
    const NkUiDrawList *list;
    uint32_t i;
    nk_ui_begin(ui, now_s);
    (void)nk_ui_top_bar(ui, 95u, sections, 3u, NULL, false);
    list = nk_ui_end(ui);
    for (i = 0u; list != NULL && i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_RECTANGLE && cmd->rgba_pm == theme->accent_dark &&
            cmd->bounds[3] - cmd->bounds[1] == 3) {
            return cmd->bounds[0];
        }
    }
    return INT_MIN;
}

/* The accent rule slides to the newly active tab over NK_UI_MOTION_BASE
   instead of jumping; Reduce motion makes it a cut. */
static int test_tab_rule_slides(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int from = INT_MIN;
    int mid;
    int to = INT_MIN;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create tab rule context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) to = draw_tab_bar(ui, 30.0 + frame * 0.01, 2);
    for (frame = 0; frame < 4; ++frame) from = draw_tab_bar(ui, 31.0 + frame * 0.5, 1);
    CHECK(from != INT_MIN && to != INT_MIN && to > from && !nk_ui_in_motion(ui),
          "the rule rests under the active tab");
    (void)draw_tab_bar(ui, 33.0, 2);
    mid = draw_tab_bar(ui, 33.0 + NK_UI_MOTION_BASE / 2.0, 2);
    CHECK(mid > from && mid < to && nk_ui_in_motion(ui),
          "half way through, the rule is between the two tabs");
    for (frame = 1; frame <= 3; ++frame) mid = draw_tab_bar(ui, 33.0 + NK_UI_MOTION_BASE * 2.0 * frame, 2);
    CHECK(mid == to && !nk_ui_in_motion(ui), "then it rests under the new tab");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    (void)draw_tab_bar(ui, 35.0, 1);
    CHECK(draw_tab_bar(ui, 35.0 + NK_UI_MOTION_BASE / 2.0, 1) == from && !nk_ui_in_motion(ui),
          "Reduce motion: the rule is under the new tab at once");
    nk_ui_destroy(ui);
    return 1;
}

/* How long a host may wait before the next frame: until the waves' next 30 Hz
   step while they play (the launcher waited 50 ms, so they stepped at 20 Hz),
   one display frame while something moves, the idle wait otherwise and under
   Reduce motion, and never past the idle wait. */
static int test_wait_follows_motion(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create wait context");
    set_test_window(ui);
    nk_ui_begin(ui, 40.0);
    CHECK(nk_ui_end(ui) != NULL, "draw a still frame");
    CHECK(nk_ui_wait_s(ui, 40.0, 0.05) == 0.05, "nothing moving: the idle wait");
    nk_ui_begin(ui, 40.01);
    CHECK(nk_ui_waves_element(ui, 9u, (NkUiRect){0.0f, 0.0f, 64.0f, 64.0f}, 0u), "queue the waves");
    CHECK(nk_ui_end(ui) != NULL, "draw a frame with the waves");
    CHECK(fabs(nk_ui_wait_s(ui, 40.01, 0.05) - (1201.0 / 30.0 - 40.01)) < 1e-9,
          "the waves: until their next 30 Hz step");
    CHECK(nk_ui_wait_s(ui, 40.01, 0.01) == 0.01, "never past the idle wait");
    CHECK(nk_ui_wait_s(ui, 40.2, 0.05) == 0.0, "a step already due: no wait");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    CHECK(nk_ui_wait_s(ui, 40.01, 0.05) == 0.05, "Reduce motion holds the waves: the idle wait");
    prefs.reduce_motion = false;
    nk_ui_set_prefs(ui, &prefs);
    nk_ui_focus(ui, 81u);
    for (frame = 0; frame < 3; ++frame) (void)draw_ring_pair(ui, 41.0 + frame * 0.01);
    nk_ui_focus(ui, 82u);
    (void)draw_ring_pair(ui, 41.05);
    (void)draw_ring_pair(ui, 41.06);
    CHECK(nk_ui_in_motion(ui) && nk_ui_wait_s(ui, 41.06, 0.05) == NK_UI_MOTION_FRAME,
          "a ring still moving: one display frame");
    nk_ui_destroy(ui);
    return 1;
}

/* An art area, 160 x 90, holding `image` when there is one, drawn at `now_s`.
   Returns the alpha the picture is drawn at, -1 when no picture is drawn. */
static int draw_art_area(NkUi *ui, double now_s, const NkUiImage *image) {
    NkUiLayout area;
    const NkUiDrawList *list;
    uint32_t i;
    memset(&area, 0, sizeof(area));
    area.width = (NkUiSizing){NK_UI_SIZE_FIXED, 160.0f, 0.0f, 0.0f};
    area.height = (NkUiSizing){NK_UI_SIZE_FIXED, 90.0f, 0.0f, 0.0f};
    nk_ui_begin(ui, now_s);
    if (nk_ui_begin_container(ui, 97u, &area, NULL)) {
        if (image != NULL) {
            NkUiImageArt art;
            memset(&art, 0, sizeof(art));
            art.cover = image;
            (void)nk_ui_image_art(ui, 98u, (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f},
                                  (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f}, &art);
        } else {
            nk_ui_label(ui, 99u, "No picture", 0, 12);
        }
        nk_ui_end_container(ui);
    }
    list = nk_ui_end(ui);
    for (i = 0u; list != NULL && i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_CUSTOM && cmd->custom_kind == NK_UI_CUSTOM_IMAGE) {
            return (int)(cmd->rgba_pm & 0xFFu);
        }
    }
    return -1;
}

/* A disc's picture read after its card is on screen eases in over
   NK_UI_MOTION_SLOW; one that comes with its card is there at once, and so is
   any under Reduce motion. */
static int test_art_fades_in(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    static uint8_t pixels[2u * 2u * 4u] = {
        255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255
    };
    NkUiImage image = {2u, 2u, pixels, 1u};
    NkUiConfig config;
    NkUiPrefs prefs;
    NkUi *ui;
    int mid;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create art fade context");
    set_test_window(ui);
    CHECK(draw_art_area(ui, 50.0, &image) == 255 && !nk_ui_in_motion(ui),
          "a picture that comes with its card is there at once");
    for (frame = 0; frame < 2; ++frame) (void)draw_art_area(ui, 50.1 + frame * 0.01, NULL);
    CHECK(draw_art_area(ui, 50.2, &image) == 0 && nk_ui_in_motion(ui),
          "a picture read after its card was drawn starts transparent");
    mid = draw_art_area(ui, 50.2 + NK_UI_MOTION_SLOW / 2.0, &image);
    CHECK(mid > 0 && mid < 255 && nk_ui_in_motion(ui), "half way through, it is fading in");
    CHECK(draw_art_area(ui, 50.2 + NK_UI_MOTION_SLOW, &image) == 255, "then it is whole");
    (void)draw_art_area(ui, 50.8, &image);
    CHECK(!nk_ui_in_motion(ui), "and still");
    memset(&prefs, 0, sizeof(prefs));
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
    (void)draw_art_area(ui, 51.0, NULL);
    CHECK(draw_art_area(ui, 51.1, &image) == 255 && !nk_ui_in_motion(ui),
          "Reduce motion: the picture is there at once");
    nk_ui_destroy(ui);
    return 1;
}

/* Clay's layout inspector draws its panel, listing the layout's elements by
   their id strings, while it is on, and the frame is still drawn. */
static int test_layout_debug(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    char dump[65536];
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create inspector context");
    set_test_window(ui);
    for (frame = 0; frame < 2; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "nk_ui.container") == NULL, "no inspector by default");
    nk_ui_set_layout_debug(ui, true);
    for (frame = 0; frame < 2; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    CHECK(nk_ui_dump(ui, dump, sizeof(dump)) < sizeof(dump) &&
              strstr(dump, "nk_ui.container") != NULL,
          "the inspector lists the layout's elements");
    nk_ui_set_layout_debug(ui, false);
    for (frame = 0; frame < 2; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    (void)nk_ui_dump(ui, dump, sizeof(dump));
    CHECK(strstr(dump, "nk_ui.container") == NULL, "and is gone once turned off");
    nk_ui_destroy(ui);
    return 1;
}

/* Clay is told the pointer once a frame: a press reaches Clay as this frame's,
   which the inspector's clicks and Clay's own hover handlers rely on. */
static int test_pointer_state_once_a_frame(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUi *ui;
    Clay_PointerDataInteractionState state;
    int frame;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create pointer state context");
    set_test_window(ui);
    for (frame = 0; frame < 3; ++frame) (void)draw_button_pair(ui, 10u, 0u);
    push_pointer(ui, 10.0f, 10.0f, true);
    nk_ui_begin(ui, 37.0);
    state = Clay_GetPointerState().state;
    (void)nk_ui_end(ui);
    CHECK(state == CLAY_POINTER_DATA_PRESSED_THIS_FRAME, "Clay sees the press as this frame's");
    nk_ui_begin(ui, 37.0);
    state = Clay_GetPointerState().state;
    (void)nk_ui_end(ui);
    CHECK(state == CLAY_POINTER_DATA_PRESSED, "and as held on the next");
    nk_ui_destroy(ui);
    return 1;
}

static int test_control_owns_horizontal(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiConfig config;
    NkUiViewport viewport = {1280, 800, 1.0f, 0, 100, 0.0f, 0.0f};
    NkUiNavDir taken = NK_UI_DIR_UP;
    bool took = false;
    NkUi *ui;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create horizontal-control context");
    nk_ui_set_viewport(ui, &viewport);
    nk_ui_focus(ui, 41u);
    horizontal_row_frame(ui, &taken, &took);
    horizontal_row_frame(ui, &taken, &took);
    CHECK(nk_ui_focused(ui) == 41u, "the segmented control has focus");

    push_nav(ui, NK_UI_DIR_RIGHT);
    horizontal_row_frame(ui, &taken, &took);
    CHECK(nk_ui_focused(ui) == 41u, "right on a control that owns it keeps the focus");
    CHECK(took && taken == NK_UI_DIR_RIGHT, "the screen takes the right press for the control");
    push_nav(ui, NK_UI_DIR_LEFT);
    horizontal_row_frame(ui, &taken, &took);
    CHECK(nk_ui_focused(ui) == 41u && took && taken == NK_UI_DIR_LEFT,
          "left stays with the control too");

    nk_ui_focus(ui, 43u);
    horizontal_row_frame(ui, &taken, &took);
    push_nav(ui, NK_UI_DIR_LEFT);
    horizontal_row_frame(ui, &taken, &took);
    CHECK(!took, "a plain button's left press is a focus move, not an event");
    CHECK(nk_ui_focused(ui) == 41u, "left from a plain button still moves focus");
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

/* A PNG signature and an IHDR chunk claiming w x h, 8-bit RGBA, and nothing
   else: enough for the header check, never enough to decode. */
static size_t png_header_only(uint8_t *out, uint32_t w, uint32_t h) {
    static const uint8_t head[16] = {0x89u, 'P', 'N', 'G', '\r', '\n', 0x1Au, '\n',
                                     0u, 0u, 0u, 13u, 'I', 'H', 'D', 'R'};
    memcpy(out, head, sizeof(head));
    out[16] = (uint8_t)(w >> 24); out[17] = (uint8_t)(w >> 16);
    out[18] = (uint8_t)(w >> 8); out[19] = (uint8_t)w;
    out[20] = (uint8_t)(h >> 24); out[21] = (uint8_t)(h >> 16);
    out[22] = (uint8_t)(h >> 8); out[23] = (uint8_t)h;
    out[24] = 8u; out[25] = 6u; out[26] = 0u; out[27] = 0u; out[28] = 0u;
    memset(out + 29, 0, 4u); /* CRC, not checked by the header read */
    return 33u;
}

/* The decoder: limits are applied before decoding, failures own nothing, and
   pixels come back premultiplied. */
static int test_image_decode(void) {
    const NkUiEmbeddedAsset *glyph = nk_ui_embedded_asset("input-prompts/xbox_button_a.png");
    NkUiImageLimits limits = {64u * 1024u, 256u, 256u};
    NkUiImage image;
    NkUiImage second;
    uint8_t buffer[64];
    uint64_t first_serial;
    size_t i;
    CHECK(glyph != NULL && glyph->size > 8u, "the embedded Xbox A glyph is present");
    CHECK(nk_ui_embedded_asset("input-prompts/missing.png") == NULL &&
          nk_ui_embedded_asset(NULL) == NULL, "an unknown asset name finds nothing");
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &image) == NK_UI_IMAGE_OK,
          "embedded glyph decodes");
    CHECK(image.width == 64u && image.height == 64u && image.rgba_pm != NULL && image.serial != 0u,
          "the Kenney Default glyphs are 64 x 64");
    for (i = 0; i < (size_t)image.width * image.height; ++i) {
        const uint8_t *p = image.rgba_pm + i * 4u;
        CHECK(p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3], "pixels are premultiplied");
    }
    first_serial = image.serial;
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &second) == NK_UI_IMAGE_OK &&
          second.serial != first_serial, "every decode gets its own serial");
    {
        /* Caller-filled pixels (the VRAM viewer, #314) draw from the same serial space. */
        uint64_t own = nk_ui_image_new_serial();
        uint64_t again = nk_ui_image_new_serial();
        CHECK(own != 0u && own != first_serial && own != second.serial && again != own,
              "a caller-owned image gets a serial no decode or earlier call has");
    }
    nk_ui_image_free(&second);
    nk_ui_image_free(&image);
    CHECK(image.rgba_pm == NULL && image.width == 0u && image.serial == 0u, "free zeroes the image");
    nk_ui_image_free(NULL);

    CHECK(nk_ui_image_decode_png(NULL, 10u, &limits, &image) == NK_UI_IMAGE_EMPTY &&
          nk_ui_image_decode_png(glyph->bytes, 0u, &limits, &image) == NK_UI_IMAGE_EMPTY &&
          nk_ui_image_decode_png(glyph->bytes, glyph->size, NULL, &image) == NK_UI_IMAGE_EMPTY &&
          nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, NULL) == NK_UI_IMAGE_EMPTY,
          "missing input is EMPTY");
    memcpy(buffer, "GIF89a and some more bytes", 27u);
    CHECK(nk_ui_image_decode_png(buffer, 27u, &limits, &image) == NK_UI_IMAGE_NOT_PNG &&
          image.rgba_pm == NULL, "only PNG is accepted");
    limits.max_bytes = glyph->size - 1u;
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &image) == NK_UI_IMAGE_TOO_LARGE,
          "encoded size over the limit");
    limits.max_bytes = 64u * 1024u;
    limits.max_width = 63u;
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &image) == NK_UI_IMAGE_TOO_LARGE &&
          image.rgba_pm == NULL, "width over the limit, refused before decoding");
    limits.max_width = 256u;
    limits.max_height = 63u;
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &image) == NK_UI_IMAGE_TOO_LARGE,
          "height over the limit");
    limits.max_height = 256u;
    /* A header claiming 5000 x 5000 never reaches the pixel decoder. */
    limits.max_width = 4096u;
    limits.max_height = 4096u;
    CHECK(nk_ui_image_decode_png(buffer, png_header_only(buffer, 5000u, 5000u), &limits, &image) ==
              NK_UI_IMAGE_TOO_LARGE, "a huge header is refused by the limits");
    CHECK(nk_ui_image_decode_png(buffer, png_header_only(buffer, 16u, 16u), &limits, &image) ==
              NK_UI_IMAGE_DECODE_FAILED && image.rgba_pm == NULL,
          "a header without image data fails to decode");
    CHECK(nk_ui_image_decode_png(glyph->bytes, glyph->size / 2u, &limits, &image) ==
              NK_UI_IMAGE_DECODE_FAILED, "a truncated PNG fails to decode");
    CHECK(strcmp(nk_ui_image_status_name(NK_UI_IMAGE_TOO_LARGE), "TOO_LARGE") == 0,
          "status names");
    {
        /* Resampling averages premultiplied pixels: a 2 x 1 image of opaque
           white and transparent black becomes one half-covered white pixel. */
        uint8_t two[8] = {255u, 255u, 255u, 255u, 0u, 0u, 0u, 0u};
        uint8_t one[4];
        uint8_t up[16];
        NkUiImage source;
        source.width = 2u;
        source.height = 1u;
        source.rgba_pm = two;
        source.serial = 1u;
        CHECK(nk_ui_image_resample(&source, 1u, 1u, one) &&
              one[0] == 128u && one[1] == 128u && one[2] == 128u && one[3] == 128u,
              "area average of premultiplied pixels");
        CHECK(nk_ui_image_resample(&source, 4u, 1u, up) && up[0] == 255u && up[4] == 255u &&
              up[8] == 0u && up[15] == 0u, "growing repeats source pixels");
        CHECK(!nk_ui_image_resample(&source, 0u, 1u, one) &&
              !nk_ui_image_resample(NULL, 1u, 1u, one) &&
              !nk_ui_image_resample(&source, 1u, 1u, NULL), "bad arguments are refused");
    }
    return 1;
}

/* An image command: tinted, dumped with the image's size, never drawn for an
   empty box, refused for a missing image. */
static int test_image_command(void) {
    const NkUiEmbeddedAsset *glyph = nk_ui_embedded_asset("input-prompts/kbd_enter.png");
    NkUiImageLimits limits = {64u * 1024u, 256u, 256u};
    NkUiImage image;
    NkUiImage empty;
    NkUiDrawBuffer buffer;
    NkUiPixelRect box = {10, 20, 34, 44};
    NkUiPixelRect flat = {10, 20, 10, 44};
    char dump[256];
    memset(&buffer, 0, sizeof(buffer));
    memset(&empty, 0, sizeof(empty));
    CHECK(glyph != NULL && nk_ui_image_decode_png(glyph->bytes, glyph->size, &limits, &image) ==
              NK_UI_IMAGE_OK, "decode the Enter keycap");
    CHECK(nk_ui_draw_add_image(&buffer, &image, box, 0x80808080u, 7u), "image command");
    CHECK(nk_ui_draw_add_image(&buffer, &image, flat, 0xFFFFFFFFu, 8u) && buffer.list.ncmd == 1u,
          "an empty box draws nothing");
    CHECK(!nk_ui_draw_add_image(&buffer, NULL, box, 0xFFFFFFFFu, 9u) &&
          !nk_ui_draw_add_image(&buffer, &empty, box, 0xFFFFFFFFu, 9u), "no image, no command");
    CHECK(buffer.list.cmd[0].kind == NK_UI_DRAW_CUSTOM &&
          buffer.list.cmd[0].custom_kind == NK_UI_CUSTOM_IMAGE &&
          buffer.list.cmd[0].custom == &image, "custom image command");
    (void)nk_ui_dump_draw_list(&buffer.list, dump, sizeof(dump));
    CHECK(strcmp(dump, "00000007 IMAGE 10 20 34 44 40404080 \"64x64\"\n") == 0,
          "dump: bounds, premultiplied tint and the image size");
    nk_ui_draw_buffer_destroy(&buffer);
    nk_ui_image_free(&image);
    return 1;
}

/* The app icon: a transparent rounded corner, the ink tile, a lime block and the
   paper bar, row padding left alone, and refusals for sizes it does not draw. */
static int test_app_icon(void) {
    enum { SIZE = 64, STRIDE = SIZE * 4 + 12 };
    static unsigned char rgba[SIZE * STRIDE];
    memset(rgba, 0xAB, sizeof(rgba));
    CHECK(nk_ui_app_icon_rgba(SIZE, rgba, STRIDE), "draw the 64 px icon");
#define ICON_PIXEL(x, y) (rgba + (size_t)(y) * STRIDE + (size_t)(x) * 4u)
    CHECK(ICON_PIXEL(0, 0)[3] == 0u, "the rounded corner stays transparent");
    CHECK(memcmp(ICON_PIXEL(32, 3), "\x10\x15\x1C\xFF", 4) == 0, "ink tile above the mark");
    CHECK(memcmp(ICON_PIXEL(15, 50), "\xD7\xF2\x4A\xFF", 4) == 0, "lime block, bottom left");
    CHECK(memcmp(ICON_PIXEL(47, 32), "\xF5\xF7\xFA\xFF", 4) == 0, "paper bar, right");
    CHECK(ICON_PIXEL(SIZE, 10)[0] == 0xABu && ICON_PIXEL(SIZE, 10)[11] == 0xABu,
          "the row padding is not written");
#undef ICON_PIXEL
    CHECK(!nk_ui_app_icon_rgba(NK_UI_APP_ICON_MIN - 1, rgba, STRIDE) &&
          !nk_ui_app_icon_rgba(NK_UI_APP_ICON_MAX + 1, rgba, sizeof(rgba)) &&
          !nk_ui_app_icon_rgba(SIZE, NULL, STRIDE) &&
          !nk_ui_app_icon_rgba(SIZE, rgba, SIZE * 4 - 1), "sizes and strides it cannot draw");
    return 1;
}

/* A host's display rule for the test: the synthetic profile folder becomes "~",
   which never grows the text. */
#define SYNTH_HOME "C:\\path\\SYNTHETIC-USER"
static size_t synth_display_text(const char *in, char *out, size_t out_size) {
    size_t home_len = strlen(SYNTH_HOME);
    size_t used = 0u;
    while (*in != '\0' && used + 1u < out_size) {
        if (strncmp(in, SYNTH_HOME, home_len) == 0) {
            out[used++] = '~';
            in += home_len;
        } else {
            out[used++] = *in++;
        }
    }
    out[used] = '\0';
    return used;
}

/* Every string reaches the frame through the host's display rules: copied
   text, labels, styled text, and a clamp, which measures the rewritten text
   so that its cut never keeps part of the profile folder. */
static int test_display_text_rules(void) {
    static const char path[] = SYNTH_HOME "\\Games\\PSP\\disc.iso";
    NkUiTextBackend fixed = nk_ui_text_fixed();
    NkUiTextStyle style = nk_ui_type(NK_UI_TYPE_DATA, 0xFFFFFFFFu);
    NkUiConfig config;
    NkUiLayout layout;
    NkUi *ui;
    char dump[4096];
    const char *shown;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    config.display_text = synth_display_text;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 400.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 200.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_COLUMN;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create context with display rules");
    nk_ui_begin(ui, 0.0);
    CHECK(nk_ui_begin_container(ui, 1u, &layout, NULL), "open display rules container");
    shown = nk_ui_frame_text(ui, path);
    CHECK(strcmp(shown, "~\\Games\\PSP\\disc.iso") == 0, "frame text shows the profile as ~");
    shown = nk_ui_text_clamp(ui, path, &style, 2000.0f, 1);
    CHECK(strcmp(shown, "~\\Games\\PSP\\disc.iso") == 0, "a clamp that fits returns the rewritten text");
    shown = nk_ui_text_clamp(ui, "Copied from " SYNTH_HOME "\\Games\\PSP\\a-long-file-name.iso",
                             &style, 60.0f, 1);
    CHECK(strstr(shown, "SYNTHETIC") == NULL && strstr(shown, "C:\\path") == NULL,
          "a clamp cuts the rewritten text, never a part of the profile folder");
    nk_ui_label(ui, 2u, path, 0, 16);
    nk_ui_text(ui, 3u, "Log file: " SYNTH_HOME "\\logs\\player.txt", &style);
    nk_ui_end_container(ui);
    CHECK(nk_ui_end(ui) != NULL, "complete display rules frame");
    CHECK(nk_ui_dump(ui, dump, sizeof(dump)) < sizeof(dump), "display rules dump fits");
    CHECK(strstr(dump, "SYNTHETIC-USER") == NULL, "no drawn run carries the profile folder");
    CHECK(strstr(dump, "~\\\\Games") != NULL || strstr(dump, "~\\Games") != NULL,
          "the label run shows the rewritten path");
    nk_ui_destroy(ui);
    return 1;
}

static bool clip_is(const NkUiDrawList *list, uint32_t index, int x0, int y0, int x1, int y1) {
    const int *clip = list->cmd[index].clip;
    return clip[0] == x0 && clip[1] == y0 && clip[2] == x1 && clip[3] == y1;
}

/* Each command carries where it may draw: its bounds inside every scissor open
   around it, nested, so a backend clips by the command alone (the Vulkan
   backend's contract) and agrees with the SDL_Renderer backend's stack. */
static int test_draw_clips_nest(void) {
    NkUiDrawBuffer buffer;
    const NkUiDrawList *list;
    memset(&buffer, 0, sizeof(buffer));
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){0.0f, 0.0f, 300.0f, 300.0f}, 1.0f, 0.0f,
                              0xFFFFFFFFu, 1u), "add an unclipped rect");
    CHECK(nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){0, 0, 100, 100}, 2u) &&
              nk_ui_draw_add_scissor(&buffer, true, (NkUiPixelRect){50, 50, 200, 200}, 3u),
          "open two nested scissors");
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){80.0f, 80.0f, 70.0f, 70.0f}, 1.0f, 0.0f,
                              0xFFFFFFFFu, 4u) &&
              nk_ui_draw_add_rect(&buffer, (NkUiRect){150.0f, 150.0f, 20.0f, 20.0f}, 1.0f, 0.0f,
                                  0xFFFFFFFFu, 5u) &&
              nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){50, 50, 200, 200}, 3u) &&
              nk_ui_draw_add_rect(&buffer, (NkUiRect){80.0f, 80.0f, 70.0f, 70.0f}, 1.0f, 0.0f,
                                  0xFFFFFFFFu, 6u) &&
              nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){0, 0, 100, 100}, 2u) &&
              nk_ui_draw_add_rect(&buffer, (NkUiRect){80.0f, 80.0f, 70.0f, 70.0f}, 1.0f, 0.0f,
                                  0xFFFFFFFFu, 7u),
          "draw inside, across and after the scissors");
    list = &buffer.list;
    CHECK(list->ncmd == 9u, "nine commands");
    CHECK(clip_is(list, 0u, 0, 0, 300, 300), "a command outside every scissor keeps its bounds");
    CHECK(clip_is(list, 1u, 0, 0, 100, 100) && clip_is(list, 2u, 50, 50, 100, 100),
          "a nested scissor is narrowed by the one around it");
    CHECK(clip_is(list, 3u, 80, 80, 100, 100), "a rect is clipped by both");
    CHECK(list->cmd[4].clip[2] <= list->cmd[4].clip[0], "a rect they hide is empty");
    CHECK(clip_is(list, 5u, 50, 50, 100, 100) && clip_is(list, 6u, 80, 80, 100, 100),
          "closing the inner scissor leaves the outer one");
    CHECK(clip_is(list, 8u, 80, 80, 150, 150), "closing both leaves the bounds");
    CHECK(!nk_ui_draw_add_scissor(&buffer, false, (NkUiPixelRect){0, 0, 1, 1}, 9u),
          "a scissor end with none open is refused");
    nk_ui_draw_buffer_reset(&buffer);
    CHECK(nk_ui_draw_add_rect(&buffer, (NkUiRect){0.0f, 0.0f, 10.0f, 10.0f}, 1.0f, 0.0f,
                              0xFFFFFFFFu, 1u) && clip_is(&buffer.list, 0u, 0, 0, 10, 10),
          "a reset buffer has no scissor open");
    nk_ui_draw_buffer_destroy(&buffer);
    return 1;
}

/* Every Clay error type has a name, and only the ones that leave a whole
   layout keep their frame. A type Clay adds later is fatal until it is named. */
static int test_layout_error_kinds(void) {
    static const struct {
        int type;
        const char *kind;
        bool fatal;
    } expected[] = {
        {CLAY_ERROR_TYPE_TEXT_MEASUREMENT_FUNCTION_NOT_PROVIDED,
         "TEXT_MEASUREMENT_FUNCTION_NOT_PROVIDED", true},
        {CLAY_ERROR_TYPE_ARENA_CAPACITY_EXCEEDED, "ARENA_CAPACITY_EXCEEDED", true},
        {CLAY_ERROR_TYPE_ELEMENTS_CAPACITY_EXCEEDED, "ELEMENTS_CAPACITY_EXCEEDED", true},
        {CLAY_ERROR_TYPE_TEXT_MEASUREMENT_CAPACITY_EXCEEDED,
         "TEXT_MEASUREMENT_CAPACITY_EXCEEDED", true},
        {CLAY_ERROR_TYPE_DUPLICATE_ID, "DUPLICATE_ID", false},
        {CLAY_ERROR_TYPE_FLOATING_CONTAINER_PARENT_NOT_FOUND,
         "FLOATING_CONTAINER_PARENT_NOT_FOUND", false},
        {CLAY_ERROR_TYPE_PERCENTAGE_OVER_1, "PERCENTAGE_OVER_1", false},
        {CLAY_ERROR_TYPE_INTERNAL_ERROR, "INTERNAL_ERROR", true},
        {CLAY_ERROR_TYPE_UNBALANCED_OPEN_CLOSE, "UNBALANCED_OPEN_CLOSE", true},
        {CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED, "HASH_MAP_CAPACITY_EXCEEDED", true},
        {CLAY_ERROR_TYPE_HASH_MAP_CAPACITY_EXCEEDED + 1, "UNKNOWN", true}
    };
    size_t i;
    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); ++i) {
        const char *kind = NULL;
        bool fatal = nk_ui_clay_error_kind(expected[i].type, &kind);
        if (kind == NULL || strcmp(kind, expected[i].kind) != 0 || fatal != expected[i].fatal) {
            fprintf(stderr, "FAIL: Clay error type %d is %s (%s), expected %s (%s)\n",
                    expected[i].type, kind != NULL ? kind : "(none)",
                    fatal ? "fatal" : "drawn", expected[i].kind,
                    expected[i].fatal ? "fatal" : "drawn");
            return 0;
        }
    }
    return 1;
}

typedef struct {
    int count;
    int fatal;
    char kinds[256];   /* every kind reported, space-separated */
    bool all_have_text;
} LayoutErrorLog;

static void record_layout_error(void *user, const NkUiLayoutError *error) {
    LayoutErrorLog *log = (LayoutErrorLog *)user;
    size_t used = strlen(log->kinds);
    ++log->count;
    if (error->fatal) ++log->fatal;
    (void)snprintf(log->kinds + used, sizeof(log->kinds) - used, "%s ", error->kind);
    if (error->text == NULL || error->text_length == 0u) log->all_have_text = false;
}

/* One frame of `spacers` spacers under a root, the first `duplicates` of them
   sharing one id. Returns whether nk_ui_end kept the frame. */
static bool layout_error_frame(NkUi *ui, double now_s, size_t spacers, size_t duplicates) {
    NkUiLayout layout;
    size_t i;
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_GROW, 0.0f, 0.0f, 0.0f};
    layout.direction = NK_UI_LAYOUT_COLUMN;
    nk_ui_begin(ui, now_s);
    if (nk_ui_begin_container(ui, 1u, &layout, NULL)) {
        for (i = 0; i < spacers; ++i) {
            NkUiId id = i < duplicates ? 2u : (NkUiId)(100u + i);
            nk_ui_spacer(ui, id, (NkUiSizing){NK_UI_SIZE_FIXED, 4.0f, 0.0f, 0.0f},
                         (NkUiSizing){NK_UI_SIZE_FIXED, 1.0f, 0.0f, 0.0f});
        }
        nk_ui_end_container(ui);
    }
    return nk_ui_end(ui) != NULL;
}

/* A duplicate id still lays out a whole frame: it is drawn and reported once
   when it starts, not every frame it repeats. Past Clay's element limit the
   layout is incomplete, so that frame is dropped, and the next whole frame is
   drawn again. */
static int test_layout_errors_reported(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    LayoutErrorLog log;
    NkUiConfig config;
    NkUi *ui;
    memset(&log, 0, sizeof(log));
    log.all_have_text = true;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    config.report_error = record_layout_error;
    config.report_user = &log;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create context with an error reporter");
    CHECK(layout_error_frame(ui, 1.0, 3u, 2u), "a frame with a duplicate id is drawn");
    CHECK(log.count == 1 && strcmp(log.kinds, "DUPLICATE_ID ") == 0 && log.fatal == 0 &&
          log.all_have_text, "the duplicate id is reported as drawn, with Clay's text");
    CHECK(layout_error_frame(ui, 2.0, 3u, 2u) && log.count == 1,
          "a duplicate id that repeats the next frame is not reported again");
    CHECK(layout_error_frame(ui, 3.0, 3u, 0u) && log.count == 1, "a clean frame reports nothing");
    CHECK(layout_error_frame(ui, 4.0, 3u, 2u) && log.count == 2,
          "a duplicate id that comes back is reported again");
    /* Clay raises no error of its own past the limit; it also stops closing
       elements, so the frame is unbalanced too. */
    CHECK(!layout_error_frame(ui, 5.0, 9000u, 0u), "a frame past the element limit is dropped");
    CHECK(strstr(log.kinds, "ELEMENTS_CAPACITY_EXCEEDED ") != NULL &&
          log.fatal == log.count - 2 && log.all_have_text,
          "the element limit is reported as fatal");
    {
        int reported = log.count;
        CHECK(layout_error_frame(ui, 6.0, 3u, 0u) && log.count == reported,
              "the next whole frame is drawn again");
    }
    nk_ui_destroy(ui);
    return 1;
}

/* nk_ui draws pictures as its own custom elements. A Clay image element (here
   declared straight through Clay, as nothing in nk_ui does) is a command the
   draw list cannot carry: the frame is dropped and reported, never drawn
   with the picture silently missing. */
static int test_undrawn_command_is_reported(void) {
    NkUiTextBackend fixed = nk_ui_text_fixed();
    LayoutErrorLog log;
    NkUiConfig config;
    NkUiLayout layout;
    NkUi *ui;
    int picture = 0;
    memset(&log, 0, sizeof(log));
    log.all_have_text = true;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &fixed;
    config.report_error = record_layout_error;
    config.report_user = &log;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create undrawn-command context");
    set_test_window(ui);
    memset(&layout, 0, sizeof(layout));
    layout.width = (NkUiSizing){NK_UI_SIZE_FIXED, 100.0f, 0.0f, 0.0f};
    layout.height = (NkUiSizing){NK_UI_SIZE_FIXED, 100.0f, 0.0f, 0.0f};
    nk_ui_begin(ui, 37.0);
    if (nk_ui_begin_container(ui, 1u, &layout, NULL)) {
        Clay_ElementDeclaration image;
        memset(&image, 0, sizeof(image));
        image.layout.sizing.width = CLAY_SIZING_FIXED(10.0f);
        image.layout.sizing.height = CLAY_SIZING_FIXED(10.0f);
        image.image.imageData = &picture;
        Clay__OpenElementWithId(Clay_GetElementId(CLAY_STRING("test.image")));
        Clay__ConfigureOpenElement(image);
        Clay__CloseElement();
        nk_ui_end_container(ui);
    }
    CHECK(nk_ui_end(ui) == NULL, "a frame with a Clay image element is dropped");
    CHECK(log.count == 1 && strcmp(log.kinds, "UNDRAWN_RENDER_COMMAND ") == 0 && log.fatal == 1 &&
          log.all_have_text, "and the command it could not draw is reported");
    nk_ui_destroy(ui);
    return 1;
}

int main(int argc, char **argv) {
    char actual[32768];
    size_t actual_size;
    if (!test_no_platform_headers() || !test_theme_tokens() || !test_scale_model() ||
        !test_explicit_ui_scale_values() || !test_typed_prefs_and_events() ||
        !test_theme_access() || !test_overlay_rejects_waves() ||
        !test_container_return_contract() || !test_backend_declarations() ||
        !test_rounding_edges() || !test_progress_sweep() || !test_focused_button_activation() ||
        !test_input_waits_for_a_new_screen_layout() ||
        !test_click_needs_press_and_release_on_one_control() ||
        !test_control_owns_horizontal() || !test_wave_caps() ||
        !test_image_decode() || !test_image_command() || !test_app_icon() ||
        !test_display_text_rules() || !test_layout_error_kinds() ||
        !test_layout_errors_reported() || !test_pointer_hover_and_press() ||
        !test_pointer_respects_clip() || !test_scroll_container() ||
        !test_draw_clips_nest() || !test_modal_card_floats() || !test_focus_ring_floats() ||
        !test_focus_ring_moves() || !test_hover_fill_eases() || !test_tab_rule_slides() ||
        !test_wait_follows_motion() || !test_art_fades_in() ||
        !test_layout_debug() || !test_pointer_state_once_a_frame() ||
        !test_undrawn_command_is_reported()) {
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
    puts("nk_ui core goldens, pixel rounding, SDL/Vulkan boundary, wave caps and image decoding passed");
    return 0;
}
