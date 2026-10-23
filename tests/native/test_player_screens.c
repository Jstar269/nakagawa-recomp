/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"
#include "nk_ui_text.h"
#include "nk_ui_theme.h"
#include "nk_ui_widgets.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NK_UI_SCREEN_CAPTURE)
#include <SDL3/SDL.h>
#include "backend/nk_ui_sdlr.h"
#endif

#define DUMP_CAP 1048576u
#define CASE_DUMP_CAP 131072u

static int failures;
static const uint8_t k_test_glyph_pixel = 0xFFu;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
        ++failures; \
    } \
} while (0)

static bool screen_test_raster(void *user, int font, int px, uint32_t glyph,
                               NkUiGlyphBitmap *out) {
    (void)user;
    (void)font;
    (void)glyph;
    if (out == NULL || px <= 0) return false;
    memset(out, 0, sizeof(*out));
    out->pixels = &k_test_glyph_pixel;
    out->width = 1;
    out->height = 1;
    out->pitch = 1;
    out->bearing_y = 1;
    out->advance_x = (float)px * 0.5f;
    return true;
}

static NkUiTextBackend screen_test_text_backend(void) {
    NkUiTextBackend backend = nk_ui_text_fixed();
    backend.raster = screen_test_raster;
    return backend;
}

static void set_test_viewport(NkUi *ui, int width, int height) {
    NkUiViewport viewport = {width, height, 1.0f, 100, 100};
    nk_ui_set_viewport(ui, &viewport);
}

static const PlayerUiTitle k_titles[] = {
    {"Synthetic Showcase Scene", "TEST00007", "Ready", "",
     "Partially works", "Showcase profile-zero acceptance remains open.", "",
     "PLAY", "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true,
     NK_UI_ART_SHOWCASE_SCENE, NULL},
    {"Synthetic Showcase Breakout", "TEST00008", "Needs setup", "",
     "Partially works", "Showcase profile-zero acceptance remains open.", "",
     "BUILD PACKAGE", "▲", 0xDBB024FFu, PLAYER_UI_PRIMARY_BUILD, true, true,
     NK_UI_ART_SHOWCASE_BREAKOUT, NULL},
    {"Synthetic Experimental Fixture", "TEST00006", "Experimental", "",
     "Experimental", "Source-owned compatibility fixture.", "", "RETRY PACKAGE CHECK",
     "◆", 0x6CB6FFFFu, PLAYER_UI_PRIMARY_RETRY_PACKAGE_CHECK, true, true, 0u, NULL},
    {"Synthetic Untested Fixture", "TEST00005", "Untested", "",
     "Untested", "No compatibility record exists.", "", "BUILD PACKAGE",
     "?", 0x6CB6FFFFu, PLAYER_UI_PRIMARY_BUILD, true, true, 0u, NULL},
    {"Synthetic Phase Five", "TEST00004", "Needs setup", "",
     "Boots to a named boundary", "Player launch stops at the package boundary.", "",
     "BUILD PACKAGE", "▲", 0xDBB024FFu, PLAYER_UI_PRIMARY_BUILD, true, true, 0u, NULL},
    {"Synthetic Title Two", "TEST00002", "Needs setup", "",
     "Works", "Source-owned runtime fixture.", "", "BUILD PACKAGE",
     "▲", 0xDBB024FFu, PLAYER_UI_PRIMARY_BUILD, true, true, 0u, NULL},
    {"Synthetic Profile", "TEST00001", "Ready", "",
     "Works", "Source-owned runtime fixture.", "", "PLAY",
     "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true, 0u, NULL}
};

static const char *const k_compatibility_words[] = {
    "Works",
    "Partially works",
    "Boots to a named boundary",
    "Not started",
    "Experimental",
    "Untested"
};

static bool append_text(char *out, size_t cap, size_t *used,
                        const char *text, size_t length) {
    if (out == NULL || used == NULL || text == NULL ||
        length > cap || *used > cap - length) {
        return false;
    }
    memcpy(out + *used, text, length);
    *used += length;
    return true;
}

static const PlayerUiAboutModel *g_about_case;

static bool append_case(char *out, size_t cap, size_t *used,
                        const char *name, NkUi *ui,
                        const PlayerUiLibraryModel *library,
                        const PlayerUiTitleModel *title) {
    const NkUiDrawList *draw;
    size_t dumped;
    static char dump[CASE_DUMP_CAP];
    int written = snprintf(dump, sizeof(dump), "--- %s ---\n", name);
    if (written < 0 || (size_t)written >= sizeof(dump) ||
        !append_text(out, cap, used, dump, (size_t)written)) {
        return false;
    }
    nk_ui_begin(ui, 0.0);
    if (library != NULL) {
        (void)player_ui_screen_library(ui, library);
    } else if (title != NULL) {
        (void)player_ui_screen_title(ui, title);
    } else {
        (void)player_ui_screen_about(ui, g_about_case);
    }
    draw = nk_ui_end(ui);
    if (draw == NULL) return false;
    dumped = nk_ui_dump(ui, dump, sizeof(dump));
    while (dumped > 0u &&
           (dump[dumped - 1u] == '\n' || dump[dumped - 1u] == '\r')) {
        --dumped;
    }
    if (dumped >= sizeof(dump) ||
        !append_text(out, cap, used, dump, dumped) ||
        !append_text(out, cap, used, "\n", 1u)) {
        return false;
    }
    return true;
}

static bool build_library_golden(char *out, size_t cap, size_t *used) {
    NkUiConfig config;
    NkUi *ui;
    PlayerUiLibraryModel model;
    NkUiTextBackend text = screen_test_text_backend();
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    if (ui == NULL) return false;
    set_test_viewport(ui, 1280, 800);
    memset(&model, 0, sizeof(model));
    model.page_size = player_ui_library_page_size(1280.0f, 800.0f, &model.columns);
    model.logical_width = 1280.0f;
    model.logical_height = 800.0f;
    CHECK(model.columns == 4u && model.page_size == 8u,
          "a 1280x800 window shows two rows of four cards");
    if (!append_case(out, cap, used, "library-empty", ui, &model, NULL)) {
        nk_ui_destroy(ui);
        return false;
    }
    model.titles = &k_titles[0];
    model.title_count = 1u;
    if (!append_case(out, cap, used, "library-one", ui, &model, NULL)) {
        nk_ui_destroy(ui);
        return false;
    }
    model.titles = k_titles;
    model.title_count = sizeof(k_titles) / sizeof(k_titles[0]);
    model.page_label = "Page 1 of 2";
    if (!append_case(out, cap, used, "library-many-statuses", ui, &model, NULL) ||
        !append_case(out, cap, used, "library-many-statuses-layout", ui,
                     &model, NULL)) {
        nk_ui_destroy(ui);
        return false;
    }
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(0u));
    nk_ui_begin(ui, 0.0);
    (void)player_ui_screen_library(ui, &model);
    if (nk_ui_end(ui) == NULL) {
        nk_ui_destroy(ui);
        return false;
    }
    {
        NkUiEvent move;
        memset(&move, 0, sizeof(move));
        move.type = NK_UI_EV_NAV;
        move.dir = NK_UI_DIR_RIGHT;
        nk_ui_push_event(ui, &move);
    }
    if (!append_case(out, cap, used, "library-focus-moved", ui, &model, NULL)) {
        nk_ui_destroy(ui);
        return false;
    }
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_LIBRARY_ITEM(1u),
          "right navigation moves focus to the adjacent title card");
    /* The prompt bar in each input mode: keyboard and mouse, then a pad. */
    {
        NkUiPrefs prefs;
        memset(&prefs, 0, sizeof(prefs));
        prefs.prompt_style = NK_UI_PROMPT_KEYCAPS;
        nk_ui_set_prefs(ui, &prefs);
        if (!append_case(out, cap, used, "library-prompts-keyboard", ui, &model, NULL)) {
            nk_ui_destroy(ui);
            return false;
        }
        prefs.prompt_style = NK_UI_PROMPT_LETTERS_A_SOUTH;
        nk_ui_set_prefs(ui, &prefs);
        if (!append_case(out, cap, used, "library-prompts-controller", ui, &model, NULL)) {
            nk_ui_destroy(ui);
            return false;
        }
        prefs.prompt_style = NK_UI_PROMPT_NEUTRAL;
        nk_ui_set_prefs(ui, &prefs);
    }
    nk_ui_destroy(ui);
    return true;
}

static bool build_title_golden(char *out, size_t cap, size_t *used) {
    NkUiConfig config;
    NkUi *ui;
    PlayerUiTitle fixture = k_titles[0];
    PlayerUiTitleModel model;
    NkUiTextBackend text = screen_test_text_backend();
    memset(&model, 0, sizeof(model));
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    if (ui == NULL) return false;
    set_test_viewport(ui, 1280, 800);
    model.title = &fixture;
    model.title_index = 0u;
    for (size_t i = 0u;
         i < sizeof(k_compatibility_words) / sizeof(k_compatibility_words[0]);
         ++i) {
        char name[96];
        fixture.compatibility_word = k_compatibility_words[i];
        fixture.compatibility_detail = "Compatibility text is supplied as data.";
        snprintf(name, sizeof(name), "title-compatibility-%zu", i);
        if (!append_case(out, cap, used, name, ui, NULL, &model)) {
            nk_ui_destroy(ui);
            return false;
        }
    }
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_SAVE_FOLDER);
    if (!append_case(out, cap, used, "title-save-folder-focused", ui,
                     NULL, &model)) {
        nk_ui_destroy(ui);
        return false;
    }
    CHECK(nk_ui_focused(ui) == PLAYER_UI_ID_TITLE_SAVE_FOLDER,
          "the save-folder action can keep focus");
    {
        /* The About page, with a fixed build date (SOURCE_DATE_EPOCH-style). */
        char line[64];
        PlayerUiAboutModel about;
        player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL, line, sizeof(line));
        memset(&about, 0, sizeof(about));
        about.version_line = line;
        about.build_commit = "0123abc";
        about.logical_width = 1280.0f;
        about.logical_height = 800.0f;
        g_about_case = &about;
        nk_ui_focus(ui, PLAYER_UI_ID_ABOUT_NOTICES);
        if (!append_case(out, cap, used, "about", ui, NULL, NULL)) {
            nk_ui_destroy(ui);
            return false;
        }
        CHECK(strstr(out, "\"v0.0.1 (10-23-2026)\"") != NULL,
              "the About page draws the version line exactly");
        g_about_case = NULL;
    }
    nk_ui_destroy(ui);
    return true;
}

/* The version line: one version, the build date as MM-DD-YYYY in UTC from the
   build epoch (SOURCE_DATE_EPOCH or the commit time), "dev build" without one. */
static void test_version_line(void) {
    char out[64];
    player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL, out, sizeof(out));
    CHECK(strcmp(out, "v0.0.1 (10-23-2026)") == 0, "the owner's example line");
    player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL + 86399LL, out, sizeof(out));
    CHECK(strcmp(out, "v0.0.1 (10-23-2026)") == 0, "the last second of the day is the same date");
    player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL + 86400LL, out, sizeof(out));
    CHECK(strcmp(out, "v0.0.1 (10-24-2026)") == 0, "midnight UTC starts the next date");
    player_ui_version_line(NK_PLAYER_VERSION, 951782400LL, out, sizeof(out));
    CHECK(strcmp(out, "v0.0.1 (02-29-2000)") == 0, "a leap day");
    player_ui_version_line(NK_PLAYER_VERSION, 0LL, out, sizeof(out));
    CHECK(strcmp(out, "v0.0.1 (01-01-1970)") == 0, "the epoch itself");
    player_ui_version_line(NK_PLAYER_VERSION, -1LL, out, sizeof(out));
    CHECK(strcmp(out, "v0.0.1 (dev build)") == 0, "no build epoch says dev build");
    CHECK(!player_ui_build_date(1792713600LL, out, 8u), "a short buffer is refused");
}

static bool write_golden(const char *path, const char *contents, size_t size) {
    FILE *file = fopen(path, "wb");
    bool ok;
    if (file == NULL) return false;
    ok = fwrite(contents, 1u, size, file) == size;
    if (fclose(file) != 0) ok = false;
    return ok;
}

static bool read_golden(const char *path, char *out, size_t cap,
                        size_t *out_size) {
    FILE *file = fopen(path, "rb");
    size_t size;
    if (file == NULL) return false;
    size = fread(out, 1u, cap, file);
    if (ferror(file) || !feof(file)) {
        fclose(file);
        return false;
    }
    if (fclose(file) != 0) return false;
    *out_size = size;
    return true;
}

static void compare_golden(const char *path, const char *actual,
                           size_t actual_size) {
    static char expected[DUMP_CAP];
    size_t expected_size = 0u;
    if (!read_golden(path, expected, sizeof(expected), &expected_size)) {
        fprintf(stderr, "FAIL: could not read screen golden %s\n", path);
        ++failures;
        return;
    }
    if (expected_size != actual_size ||
        memcmp(expected, actual, expected_size < actual_size
            ? expected_size : actual_size) != 0) {
        size_t mismatch = 0u;
        size_t limit = expected_size < actual_size ? expected_size : actual_size;
        while (mismatch < limit && expected[mismatch] == actual[mismatch]) {
            ++mismatch;
        }
        fprintf(stderr, "FAIL: %s dump changed at byte %zu (expected=%zu actual=%zu)\n",
                path, mismatch, expected_size, actual_size);
        ++failures;
    }
}

static void test_router(void) {
    NkUiRouter *router = nk_ui_router_create();
    NkUiEvent back;
    CHECK(router != NULL, "router allocates");
    if (router == NULL) return;
    CHECK(nk_ui_router_current(router) == PLAYER_UI_ROUTE_LIBRARY,
          "router starts at Library");
    CHECK(nk_ui_router_depth(router) == 1u, "router has one root screen");
    nk_ui_router_set_focus(router, PLAYER_UI_ID_LIBRARY_ITEM(3u));
    CHECK(nk_ui_router_push(router, PLAYER_UI_ROUTE_TITLE),
          "router pushes Title detail");
    CHECK(nk_ui_router_current(router) == PLAYER_UI_ROUTE_TITLE,
          "pushed screen is current");
    CHECK(nk_ui_router_depth(router) == 2u, "push keeps the Library underneath");
    nk_ui_router_set_focus(router, PLAYER_UI_ID_TITLE_SAVE_FOLDER);
    memset(&back, 0, sizeof(back));
    back.type = NK_UI_EV_BACK;
    CHECK(nk_ui_router_handle_event(router, &back),
          "Back event is consumed by the router");
    CHECK(nk_ui_router_current(router) == PLAYER_UI_ROUTE_LIBRARY,
          "Back pops to Library");
    CHECK(nk_ui_router_take_focus_restore(router) ==
          PLAYER_UI_ID_LIBRARY_ITEM(3u), "pop restores the saved Library focus");
    CHECK(nk_ui_router_take_focus_restore(router) == 0u,
          "focus restore is consumed once");
    CHECK(nk_ui_router_replace(router, PLAYER_UI_ROUTE_TITLE),
          "router replaces the root screen");
    CHECK(nk_ui_router_depth(router) == 1u &&
          nk_ui_router_current(router) == PLAYER_UI_ROUTE_TITLE,
          "replace discards the old stack");
    CHECK(nk_ui_router_handle_event(router, &back),
          "root Back is consumed without changing route");
    CHECK(nk_ui_router_current(router) == PLAYER_UI_ROUTE_TITLE,
          "Back at root leaves the route in place");
    nk_ui_router_destroy(router);
}

static PlayerUiAction activate_library_button(NkUiId id,
                                               const PlayerUiLibraryModel *model) {
    NkUiConfig config;
    NkUiEvent activate;
    NkUi *ui;
    PlayerUiAction result = {PLAYER_UI_ACTION_NONE,
                             PLAYER_UI_PRIMARY_NONE, 0u};
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    ui = nk_ui_create(&config);
    if (ui == NULL) return result;
    set_test_viewport(ui, 1280, 800);
    nk_ui_focus(ui, id);
    memset(&activate, 0, sizeof(activate));
    activate.type = NK_UI_EV_ACTIVATE;
    activate.down = true;
    nk_ui_push_event(ui, &activate);
    nk_ui_begin(ui, 0.0);
    result = player_ui_screen_library(ui, model);
    (void)nk_ui_end(ui);
    nk_ui_destroy(ui);
    return result;
}

static PlayerUiAction activate_title_button(NkUiId id,
                                             const PlayerUiTitleModel *model) {
    NkUiConfig config;
    NkUiEvent activate;
    NkUi *ui;
    PlayerUiAction result = {PLAYER_UI_ACTION_NONE,
                             PLAYER_UI_PRIMARY_NONE, 0u};
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    ui = nk_ui_create(&config);
    if (ui == NULL) return result;
    set_test_viewport(ui, 1280, 800);
    nk_ui_focus(ui, id);
    memset(&activate, 0, sizeof(activate));
    activate.type = NK_UI_EV_ACTIVATE;
    activate.down = true;
    nk_ui_push_event(ui, &activate);
    nk_ui_begin(ui, 0.0);
    result = player_ui_screen_title(ui, model);
    (void)nk_ui_end(ui);
    nk_ui_destroy(ui);
    return result;
}

static void test_screen_actions(void) {
    PlayerUiLibraryModel library;
    PlayerUiTitleModel detail;
    PlayerUiAction action;
    memset(&library, 0, sizeof(library));
    memset(&detail, 0, sizeof(detail));
    library.columns = 2u;
    library.page_size = 4u;
    library.logical_width = 640.0f;
    action = activate_library_button(PLAYER_UI_ID_LIBRARY_EMPTY_ADD, &library);
    CHECK(action.kind == PLAYER_UI_ACTION_ADD_DISC,
          "empty Library action requests a PSP disc image");
    action = activate_library_button(PLAYER_UI_ID_LIBRARY_EMPTY_WIZARD, &library);
    CHECK(action.kind == PLAYER_UI_ACTION_START_WIZARD,
          "the empty Library's main action starts the setup wizard");
    library.titles = k_titles;
    library.title_count = sizeof(k_titles) / sizeof(k_titles[0]);
    action = activate_library_button(PLAYER_UI_ID_LIBRARY_ITEM(2u), &library);
    CHECK(action.kind == PLAYER_UI_ACTION_OPEN_TITLE && action.title_index == 2u,
          "a Library card opens its matching Title detail");
    detail.title = &k_titles[0];
    detail.title_index = 0u;
    action = activate_title_button(PLAYER_UI_ID_TITLE_PRIMARY, &detail);
    CHECK(action.kind == PLAYER_UI_ACTION_PRIMARY &&
          action.primary == PLAYER_UI_PRIMARY_PLAY,
          "the primary button returns its existing Play action");
    action = activate_title_button(PLAYER_UI_ID_TITLE_SAVE_FOLDER, &detail);
    CHECK(action.kind == PLAYER_UI_ACTION_OPEN_SAVE_FOLDER,
          "the save-folder button requests the existing per-title folder");
    action = activate_title_button(PLAYER_UI_ID_TITLE_BACK, &detail);
    CHECK(action.kind == PLAYER_UI_ACTION_BACK,
          "the back button returns to the Library");
    library.page_size = 8u;
    library.columns = 4u;
    library.logical_width = 1280.0f;
    action = activate_library_button(PLAYER_UI_ID_LIBRARY_ADD, &library);
    CHECK(action.kind == PLAYER_UI_ACTION_ADD_DISC,
          "the add card at the end of the grid requests a disc image");
}

/* Design-system checks on the real layout: the focus ring sits outside the
   focused card, the primary label is centred in its button, and the card grid
   keeps its gaps. */
static const NkUiDrawCmd *find_command(const NkUiDrawList *list, NkUiId id, int kind,
                                       uint32_t rgba) {
    for (uint32_t i = 0u; list != NULL && i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->element_id == id && cmd->kind == kind &&
            (rgba == 0u || cmd->rgba_pm == rgba)) {
            return cmd;
        }
    }
    return NULL;
}

static void test_design_system_layout(void) {
    NkUiConfig config;
    NkUi *ui;
    PlayerUiLibraryModel library;
    PlayerUiTitleModel detail;
    const NkUiDrawList *draw;
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiTextBackend text = screen_test_text_backend();
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    CHECK(ui != NULL, "create the design-system probe");
    if (ui == NULL) return;
    set_test_viewport(ui, 1280, 800);
    memset(&library, 0, sizeof(library));
    library.titles = k_titles;
    library.title_count = sizeof(k_titles) / sizeof(k_titles[0]);
    library.page_size = player_ui_library_page_size(1280.0f, 800.0f, &library.columns);
    library.logical_width = 1280.0f;
    library.logical_height = 800.0f;
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(0u));
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 37.0);
        (void)player_ui_screen_library(ui, &library);
        draw = nk_ui_end(ui);
    }
    CHECK(draw != NULL, "lay out the Library");
    {
        const NkUiDrawCmd *card = find_command(draw, PLAYER_UI_ID_LIBRARY_ITEM(0u),
                                               NK_UI_DRAW_RECTANGLE, 0u);
        const NkUiDrawCmd *next = find_command(draw, PLAYER_UI_ID_LIBRARY_ITEM(1u),
                                               NK_UI_DRAW_RECTANGLE, 0u);
        const NkUiDrawCmd *ring = NULL;
        int ring_x0 = 1 << 30;
        int ring_x1 = -(1 << 30);
        for (uint32_t i = 0u; i < draw->ncmd; ++i) {
            const NkUiDrawCmd *cmd = &draw->cmd[i];
            if (cmd->element_id == PLAYER_UI_ID_LIBRARY_ITEM(0u) &&
                cmd->kind == NK_UI_DRAW_BORDER && cmd->rgba_pm == theme->focus) {
                ring = cmd;
                if (cmd->bounds[0] < ring_x0) ring_x0 = cmd->bounds[0];
                if (cmd->bounds[2] > ring_x1) ring_x1 = cmd->bounds[2];
            }
        }
        CHECK(card != NULL && next != NULL, "the first two cards are drawn");
        CHECK(ring != NULL, "the focused card carries the accent focus ring");
        if (card != NULL && ring != NULL) {
            CHECK(ring_x0 == card->bounds[0] - 6 && ring_x1 == card->bounds[2] + 6,
                  "the ring is 3 px wide, 3 px outside the card");
        }
        if (card != NULL && next != NULL) {
            CHECK(next->bounds[0] - card->bounds[2] == (int)NK_UI_TILE_GAP,
                  "cards keep the 20 px grid gap");
            CHECK(card->bounds[0] == (int)NK_UI_GUTTER, "the grid starts at the page gutter");
        }
    }
    memset(&detail, 0, sizeof(detail));
    detail.title = &k_titles[0];
    detail.logical_width = 1280.0f;
    detail.logical_height = 800.0f;
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_PRIMARY);
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(ui, 37.0);
        (void)player_ui_screen_title(ui, &detail);
        draw = nk_ui_end(ui);
    }
    CHECK(draw != NULL, "lay out Title detail");
    {
        const NkUiDrawCmd *button = find_command(draw, PLAYER_UI_ID_TITLE_PRIMARY,
                                                 NK_UI_DRAW_RECTANGLE, theme->accent_dark);
        const NkUiDrawCmd *label = find_command(draw, PLAYER_UI_ID_TITLE_PRIMARY,
                                                NK_UI_DRAW_TEXT, 0u);
        CHECK(button != NULL && label != NULL, "the primary action is drawn with its label");
        if (button != NULL && label != NULL) {
            int left = label->bounds[0] - button->bounds[0];
            int right = button->bounds[2] - label->bounds[2];
            int top = label->bounds[1] - button->bounds[1];
            int bottom = button->bounds[3] - label->bounds[3];
            CHECK(button->bounds[3] - button->bounds[1] == (int)NK_UI_CONTROL_HEIGHT_LARGE,
                  "the primary action is the 52 px large button");
            CHECK(left - right <= 1 && right - left <= 1,
                  "the label is centred across the button");
            CHECK(top - bottom <= 1 && bottom - top <= 1,
                  "the label is centred down the button, not stuck to its top");
        }
    }
    nk_ui_destroy(ui);
}

#if defined(NK_UI_SCREEN_CAPTURE) && defined(NK_UI_WITH_SDL) && \
    defined(NK_UI_WITH_SDL_TTF)
typedef struct {
    NkUi *ui;
    NkUiSdlr *backend;
    SDL_Renderer *renderer;
    SDL_Surface *surface;
    const char *directory;
} CaptureRig;

/* Lays the screen out twice (the focus graph uses the previous frame's
   rectangles), draws it and saves <directory>/<name>.bmp. */
static bool capture_one(CaptureRig *rig, const char *name,
                        const PlayerUiLibraryModel *library,
                        const PlayerUiTitleModel *title) {
    const NkUiDrawList *draw = NULL;
    char path[4096];
    const char separator =
#if defined(_WIN32) || defined(_WIN64)
        '\\';
#else
        '/';
#endif
    int written = snprintf(path, sizeof(path), "%s%c%s.bmp", rig->directory, separator, name);
    if (written <= 0 || (size_t)written >= sizeof(path)) return false;
    for (int frame = 0; frame < 2; ++frame) {
        nk_ui_begin(rig->ui, 37.0);
        if (library != NULL) {
            (void)player_ui_screen_library(rig->ui, library);
        } else if (title != NULL) {
            (void)player_ui_screen_title(rig->ui, title);
        } else {
            (void)player_ui_screen_about(rig->ui, g_about_case);
        }
        draw = nk_ui_end(rig->ui);
    }
    if (draw == NULL ||
        !SDL_SetRenderDrawColor(rig->renderer, 11u, 15u, 20u, 255u) ||
        !SDL_RenderClear(rig->renderer) ||
        !nk_ui_sdlr_render(rig->backend, draw)) {
        return false;
    }
    SDL_RenderPresent(rig->renderer);
    return SDL_SaveBMP(rig->surface, path);
}

static void capture_prefs(NkUi *ui, NkUiPromptStyle style, bool high_contrast) {
    NkUiPrefs prefs;
    memset(&prefs, 0, sizeof(prefs));
    prefs.prompt_style = style;
    prefs.high_contrast_focus = high_contrast;
    prefs.reduce_motion = true;
    nk_ui_set_prefs(ui, &prefs);
}

/* One capture at another window size and UI scale, with its own surface,
   renderer, layout core and backend (the logical layout follows the scale). */
static bool capture_scaled(const char *directory, const NkUiTextBackend *text,
                           const char *name, int pixel_w, int pixel_h, float display_scale,
                           const PlayerUiTitle *titles, size_t title_count,
                           const PlayerUiTitle *detail, NkUiId focus) {
    SDL_Surface *surface = SDL_CreateSurface(pixel_w, pixel_h, SDL_PIXELFORMAT_RGBA32);
    SDL_Renderer *renderer = surface != NULL ? SDL_CreateSoftwareRenderer(surface) : NULL;
    NkUiConfig config;
    NkUi *ui = NULL;
    NkUiSdlr *backend = NULL;
    CaptureRig rig;
    bool ok = false;
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = text;
    if (renderer != NULL) ui = nk_ui_create(&config);
    if (ui != NULL) backend = nk_ui_sdlr_create(renderer, text);
    if (backend != NULL) {
        NkUiViewport viewport = {pixel_w, pixel_h, display_scale, 0, 100};
        NkUiScale scale = nk_ui_scale_for_viewport(&viewport);
        nk_ui_set_viewport(ui, &viewport);
        nk_ui_sdlr_set_scale(backend, scale.density);
        capture_prefs(ui, NK_UI_PROMPT_KEYCAPS, false);
        rig.ui = ui;
        rig.backend = backend;
        rig.renderer = renderer;
        rig.surface = surface;
        rig.directory = directory;
        nk_ui_focus(ui, focus);
        if (detail == NULL) {
            PlayerUiLibraryModel library;
            memset(&library, 0, sizeof(library));
            library.titles = titles;
            library.title_count = title_count;
            library.page_size = player_ui_library_page_size(scale.logical_w, scale.logical_h,
                                                            &library.columns);
            library.logical_width = scale.logical_w;
            library.logical_height = scale.logical_h;
            if (library.title_count + 1u > library.page_size) library.page_label = "Page 1 of 2";
            ok = capture_one(&rig, name, &library, NULL);
        } else {
            PlayerUiTitleModel model;
            memset(&model, 0, sizeof(model));
            model.title = detail;
            model.logical_width = scale.logical_w;
            model.logical_height = scale.logical_h;
            ok = capture_one(&rig, name, NULL, &model);
        }
    }
    nk_ui_sdlr_destroy(backend);
    nk_ui_destroy(ui);
    if (renderer != NULL) SDL_DestroyRenderer(renderer);
    if (surface != NULL) SDL_DestroySurface(surface);
    return ok;
}

static int capture_screens(const char *directory) {
    SDL_Surface *surface = NULL;
    SDL_Renderer *renderer = NULL;
    NkUiTtf *ttf = NULL;
    NkUiSdlr *backend = NULL;
    NkUi *ui = NULL;
    NkUiTextBackend text;
    NkUiConfig config;
    PlayerUiLibraryModel library;
    PlayerUiTitleModel title;
    PlayerUiTitle title_fixture = k_titles[0];
    PlayerUiTitle setup_fixture = k_titles[4];
    CaptureRig rig;
    bool ok = false;

    if (SDL_Init(SDL_INIT_VIDEO) == false) {
        fprintf(stderr, "SKIP: SDL video is unavailable for screen captures: %s\n",
                SDL_GetError());
        return 77;
    }
    surface = SDL_CreateSurface(1280, 800, SDL_PIXELFORMAT_RGBA32);
    if (surface == NULL) goto done;
    renderer = SDL_CreateSoftwareRenderer(surface);
    if (renderer == NULL) goto done;
    ttf = nk_ui_ttf_create(NULL, 0u);
    if (ttf == NULL) {
        fprintf(stderr, "SKIP: SDL3_ttf is unavailable for screen captures: %s\n",
                nk_ui_ttf_last_error());
        ok = false;
        goto done;
    }
    for (int i = 0; i < NK_UI_FONT_COUNT; ++i) {
        if (!nk_ui_ttf_add_font(ttf, i, nk_ui_font_path(i))) goto done;
    }
    for (int i = 0; i < NK_UI_FONT_COUNT; ++i) {
        if (i == NK_UI_FONT_CJK) continue;
        if (i != NK_UI_FONT_BODY && !nk_ui_ttf_set_fallback(ttf, i, NK_UI_FONT_BODY)) goto done;
        if (!nk_ui_ttf_set_fallback(ttf, i, NK_UI_FONT_CJK)) goto done;
    }
    text = nk_ui_ttf_backend(ttf);
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_LAUNCHER;
    config.text = &text;
    ui = nk_ui_create(&config);
    if (ui == NULL) goto done;
    backend = nk_ui_sdlr_create(renderer, &text);
    if (backend == NULL) goto done;
    {
        NkUiViewport viewport = {1280, 800, 1.0f, 100, 100};
        nk_ui_set_viewport(ui, &viewport);
        nk_ui_sdlr_set_scale(backend, 1.0f);
    }
    rig.ui = ui;
    rig.backend = backend;
    rig.renderer = renderer;
    rig.surface = surface;
    rig.directory = directory;

    memset(&library, 0, sizeof(library));
    library.titles = k_titles;
    library.title_count = sizeof(k_titles) / sizeof(k_titles[0]);
    library.page_size = player_ui_library_page_size(1280.0f, 800.0f, &library.columns);
    library.logical_width = 1280.0f;
    library.logical_height = 800.0f;
    capture_prefs(ui, NK_UI_PROMPT_KEYCAPS, false);
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(0u));
    if (!capture_one(&rig, "library", &library, NULL)) goto done;
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ADD);
    if (!capture_one(&rig, "library-add-focused", &library, NULL)) goto done;
    capture_prefs(ui, NK_UI_PROMPT_NEUTRAL, true);
    nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(1u));
    if (!capture_one(&rig, "library-controller-high-contrast", &library, NULL)) goto done;
    capture_prefs(ui, NK_UI_PROMPT_KEYCAPS, false);
    {
        PlayerUiLibraryModel empty = library;
        empty.titles = NULL;
        empty.title_count = 0u;
        nk_ui_focus(ui, 0u);
        if (!capture_one(&rig, "library-empty", &empty, NULL)) goto done;
    }
    {
        PlayerUiLibraryModel paged = library;
        paged.page_size = 4u;
        paged.page_index = 1u;
        paged.page_label = "Page 2 of 2";
        nk_ui_focus(ui, PLAYER_UI_ID_LIBRARY_ITEM(4u));
        if (!capture_one(&rig, "library-second-page", &paged, NULL)) goto done;
    }

    memset(&title, 0, sizeof(title));
    title_fixture.compatibility_word = "Partially works";
    title_fixture.compatibility_detail =
        "Showcase profile-zero acceptance remains open.";
    title.title = &title_fixture;
    title.title_index = 0u;
    title.logical_width = 1280.0f;
    title.logical_height = 800.0f;
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_PRIMARY);
    if (!capture_one(&rig, "title", NULL, &title)) goto done;
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_SAVE_FOLDER);
    if (!capture_one(&rig, "title-save-folder-focused", NULL, &title)) goto done;
    capture_prefs(ui, NK_UI_PROMPT_LETTERS_A_SOUTH, false);
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_PRIMARY);
    if (!capture_one(&rig, "title-controller", NULL, &title)) goto done;
    capture_prefs(ui, NK_UI_PROMPT_KEYCAPS, false);
    title.title = &setup_fixture;
    title.title_index = 4u;
    setup_fixture.library_status_detail =
        "Build the package for this PC before playing. It takes a few minutes.";
    setup_fixture.input_note =
        "Nakagawa does not decrypt games; it uses decrypted files from your own copy.";
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_PRIMARY);
    if (!capture_one(&rig, "title-needs-setup", NULL, &title)) goto done;
    {
        char line[64];
        PlayerUiAboutModel about;
        player_ui_version_line(NK_PLAYER_VERSION, 1792713600LL, line, sizeof(line));
        memset(&about, 0, sizeof(about));
        about.version_line = line;
        about.build_commit = "0123abc";
        about.logical_width = 1280.0f;
        about.logical_height = 800.0f;
        g_about_case = &about;
        nk_ui_focus(ui, PLAYER_UI_ID_TAB_ABOUT);
        if (!capture_one(&rig, "about", NULL, NULL)) goto done;
        capture_prefs(ui, NK_UI_PROMPT_LETTERS_A_SOUTH, false);
        nk_ui_focus(ui, PLAYER_UI_ID_ABOUT_NOTICES);
        if (!capture_one(&rig, "about-controller", NULL, NULL)) goto done;
        capture_prefs(ui, NK_UI_PROMPT_KEYCAPS, false);
        g_about_case = NULL;
    }
    /* The same layouts at 150% on a 1920x1200 display, and in a small window. */
    if (!capture_scaled(directory, &text, "library-1920x1200-150pct", 1920, 1200, 1.5f,
                        k_titles, sizeof(k_titles) / sizeof(k_titles[0]), NULL,
                        PLAYER_UI_ID_LIBRARY_ITEM(0u)) ||
        !capture_scaled(directory, &text, "title-1920x1200-150pct", 1920, 1200, 1.5f,
                        k_titles, 0u, &title_fixture, PLAYER_UI_ID_TITLE_PRIMARY) ||
        !capture_scaled(directory, &text, "library-960x600", 960, 600, 1.0f,
                        k_titles, sizeof(k_titles) / sizeof(k_titles[0]), NULL,
                        PLAYER_UI_ID_LIBRARY_ITEM(0u)) ||
        !capture_scaled(directory, &text, "title-960x600", 960, 600, 1.0f,
                        k_titles, 0u, &title_fixture, PLAYER_UI_ID_TITLE_PRIMARY)) {
        goto done;
    }
    ok = true;
done:
    if (!ok && SDL_GetError()[0] != '\0') {
        fprintf(stderr, "FAIL: screen capture failed: %s\n", SDL_GetError());
    }
    nk_ui_sdlr_destroy(backend);
    nk_ui_destroy(ui);
    nk_ui_ttf_destroy(ttf);
    if (renderer != NULL) SDL_DestroyRenderer(renderer);
    if (surface != NULL) SDL_DestroySurface(surface);
    SDL_Quit();
    if (!ok) return 1;
    printf("synthetic Library and Title detail captures saved in %s\n", directory);
    return 0;
}
#else
static int capture_screens(const char *directory) {
    (void)directory;
    puts("SKIP: screen captures need SDL3 and SDL3_ttf headers");
    return 77;
}
#endif

int main(int argc, char **argv) {
    static char library[DUMP_CAP];
    static char title[DUMP_CAP];
    size_t library_size = 0u;
    size_t title_size = 0u;
    bool write = argc > 1 && strcmp(argv[1], "--write-goldens") == 0;
    const char *capture_dir = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strncmp(argv[i], "--capture-dir=", 14u) == 0) {
            capture_dir = argv[i] + 14u;
        }
    }
    library[0] = '\0';
    title[0] = '\0';
    test_router();
    test_screen_actions();
    test_version_line();
    test_design_system_layout();
    CHECK(build_library_golden(library, sizeof(library), &library_size),
          "build Library screen dumps");
    CHECK(build_title_golden(title, sizeof(title), &title_size),
          "build Title detail screen dumps");
    if (write && failures == 0) {
        CHECK(write_golden("fixtures/nk_ui/library.golden", library, library_size),
              "write Library screen golden");
        CHECK(write_golden("fixtures/nk_ui/title.golden", title, title_size),
              "write Title detail screen golden");
    } else if (failures == 0) {
        compare_golden("fixtures/nk_ui/library.golden", library, library_size);
        compare_golden("fixtures/nk_ui/title.golden", title, title_size);
    }
    if (capture_dir != NULL && failures == 0) {
        int capture_result = capture_screens(capture_dir);
        if (capture_result != 0 && capture_result != 77) ++failures;
    }
    if (failures != 0) return 1;
    puts("player screen snapshots and router focus tests passed");
    return 0;
}
