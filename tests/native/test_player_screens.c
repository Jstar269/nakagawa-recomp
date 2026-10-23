/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "player_ui_nk.h"
#include "nk_ui_text.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(NK_UI_SCREEN_CAPTURE)
#include <SDL3/SDL.h>
#include "backend/nk_ui_sdlr.h"
#endif

#define DUMP_CAP 262144u

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
     "PLAY NOW", "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true},
    {"Synthetic Showcase Breakout", "TEST00008", "Needs setup", "",
     "Partially works", "Showcase profile-zero acceptance remains open.", "",
     "BUILD PACKAGE", "▲", 0xDBB024FFu, PLAYER_UI_PRIMARY_BUILD, true, true},
    {"Synthetic Experimental Fixture", "TEST00006", "Experimental", "",
     "Experimental", "Source-owned compatibility fixture.", "", "RETRY PACKAGE CHECK",
     "◆", 0xDBB024FFu, PLAYER_UI_PRIMARY_RETRY_PACKAGE_CHECK, true, true},
    {"Synthetic Untested Fixture", "TEST00005", "Untested", "",
     "Untested", "No compatibility record exists.", "", "BUILD PACKAGE",
     "?", 0x6CB6FFFFu, PLAYER_UI_PRIMARY_BUILD, true, true},
    {"Synthetic Phase Five", "TEST00004", "Needs setup", "",
     "Boots to a named boundary", "Player launch stops at the package boundary.", "",
     "BUILD PACKAGE", "▲", 0xDBB024FFu, PLAYER_UI_PRIMARY_BUILD, true, true},
    {"Synthetic Title Two", "TEST00002", "Needs setup", "",
     "Works", "Source-owned runtime fixture.", "", "BUILD PACKAGE",
     "▲", 0xDBB024FFu, PLAYER_UI_PRIMARY_BUILD, true, true},
    {"Synthetic Profile", "TEST00001", "Ready", "",
     "Works", "Source-owned runtime fixture.", "", "PLAY NOW",
     "●", 0x2DD4BFFFu, PLAYER_UI_PRIMARY_PLAY, true, true}
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

static bool append_case(char *out, size_t cap, size_t *used,
                        const char *name, NkUi *ui,
                        const PlayerUiLibraryModel *library,
                        const PlayerUiTitleModel *title) {
    const NkUiDrawList *draw;
    size_t dumped;
    char dump[32768];
    int written = snprintf(dump, sizeof(dump), "--- %s ---\n", name);
    if (written < 0 || (size_t)written >= sizeof(dump) ||
        !append_text(out, cap, used, dump, (size_t)written)) {
        return false;
    }
    nk_ui_begin(ui, 0.0);
    if (library != NULL) {
        (void)player_ui_screen_library(ui, library);
    } else {
        (void)player_ui_screen_title(ui, title);
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
    model.columns = 2u;
    model.page_size = 4u;
    model.logical_width = 1280.0f;
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
    nk_ui_destroy(ui);
    return true;
}

static bool build_title_golden(char *out, size_t cap, size_t *used) {
    NkUiConfig config;
    NkUi *ui;
    PlayerUiTitle fixture = k_titles[0];
    PlayerUiTitleModel model;
    NkUiTextBackend text = screen_test_text_backend();
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
    nk_ui_destroy(ui);
    return true;
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
    char expected[DUMP_CAP];
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
    library.columns = 2u;
    library.page_size = 4u;
    library.logical_width = 640.0f;
    action = activate_library_button(PLAYER_UI_ID_LIBRARY_EMPTY_ADD, &library);
    CHECK(action.kind == PLAYER_UI_ACTION_ADD_DISC,
          "empty Library action requests a PSP disc image");
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
}

#if defined(NK_UI_SCREEN_CAPTURE) && defined(NK_UI_WITH_SDL) && \
    defined(NK_UI_WITH_SDL_TTF)
static bool capture_one(NkUi *ui, NkUiSdlr *backend, SDL_Renderer *renderer,
                        SDL_Surface *surface, const char *path,
                        const PlayerUiLibraryModel *library,
                        const PlayerUiTitleModel *title) {
    const NkUiDrawList *draw;
    nk_ui_begin(ui, 0.0);
    if (library != NULL) {
        (void)player_ui_screen_library(ui, library);
    } else {
        (void)player_ui_screen_title(ui, title);
    }
    draw = nk_ui_end(ui);
    if (draw == NULL ||
        !SDL_SetRenderDrawColor(renderer, 11u, 15u, 20u, 255u) ||
        !SDL_RenderClear(renderer) ||
        !nk_ui_sdlr_render(backend, draw)) {
        return false;
    }
    SDL_RenderPresent(renderer);
    return SDL_SaveBMP(surface, path);
}

static int capture_screens(const char *directory) {
    static const char *const font_paths[] = {
        "assets/fonts/ui/geist/Geist-Regular.ttf",
        "assets/fonts/ui/chakra-petch/ChakraPetch-SemiBold.ttf",
        "assets/fonts/ui/geist-mono/GeistMono-Regular.ttf",
        "assets/fonts/ui/noto-sans-jp/NotoSansJP-Regular.ttf"
    };
    const char separator =
#if defined(_WIN32) || defined(_WIN64)
        '\\';
#else
        '/';
#endif
    char library_path[4096];
    char title_path[4096];
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
    bool ok = false;
    int written;

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
    for (int i = 0; i < 4; ++i) {
        if (!nk_ui_ttf_add_font(ttf, i, font_paths[i])) goto done;
    }
    if (!nk_ui_ttf_set_fallback(ttf, 0, 3) ||
        !nk_ui_ttf_set_fallback(ttf, 1, 0) ||
        !nk_ui_ttf_set_fallback(ttf, 1, 3) ||
        !nk_ui_ttf_set_fallback(ttf, 2, 0) ||
        !nk_ui_ttf_set_fallback(ttf, 2, 3)) {
        goto done;
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
    memset(&library, 0, sizeof(library));
    library.titles = k_titles;
    library.title_count = sizeof(k_titles) / sizeof(k_titles[0]);
    library.page_size = 8u;
    library.columns = 4u;
    library.logical_width = 1280.0f;
    written = snprintf(library_path, sizeof(library_path), "%s%c%s",
                       directory, separator, "library.bmp");
    if (written <= 0 || (size_t)written >= sizeof(library_path)) goto done;
    written = snprintf(title_path, sizeof(title_path), "%s%c%s",
                       directory, separator, "title.bmp");
    if (written <= 0 || (size_t)written >= sizeof(title_path)) goto done;
    if (!capture_one(ui, backend, renderer, surface, library_path,
                     &library, NULL)) goto done;
    /* A second layout makes the focus graph's previous-frame rectangles real. */
    if (!capture_one(ui, backend, renderer, surface, library_path,
                     &library, NULL)) goto done;
    title_fixture.compatibility_word = "Partially works";
    title_fixture.compatibility_detail =
        "Showcase profile-zero acceptance remains open.";
    title.title = &title_fixture;
    title.title_index = 0u;
    nk_ui_focus(ui, PLAYER_UI_ID_TITLE_SAVE_FOLDER);
    if (!capture_one(ui, backend, renderer, surface, title_path,
                     NULL, &title)) goto done;
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
    char library[DUMP_CAP];
    char title[DUMP_CAP];
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
