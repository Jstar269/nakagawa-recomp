/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The in-game overlay's runtime host (#1159, C9c) on its production path: SDL3_ttf
   and the bundled UI fonts, real SDL key and pad events through nk_ui's SDL input,
   and the pages drawn by the SDL_Renderer backend into a software surface laid over
   a composed base, as the pause presenter (src/rt/gui.c) draws them.

   Exit 77 means SKIP with the reason printed: a build without the SDL3 or SDL3_ttf
   headers, or a host where the SDL3_ttf library cannot be loaded. */

#include "overlay_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define EXIT_SKIP 77

#if !defined(NK_UI_WITH_SDL) || !defined(NK_UI_WITH_SDL_TTF) || !defined(NK_UI_WITH_SDL3)
int main(void) {
    printf("SKIP overlay host: SDL3 and SDL3_ttf headers were not available at build time\n");
    return EXIT_SKIP;
}
#else

#include "backend/nk_ui_sdlr.h"

#include <SDL3/SDL.h>

static int failures;

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
        ++failures; \
    } \
} while (0)

#define BASE_RGBA 0x20304050u

typedef struct {
    SDL_Surface *surface;
    SDL_Renderer *renderer;
} Canvas;

static NkOverlayHost *g_host;
static Canvas g_canvas;
static NkOverlayModel g_model;
static NkOverlayAction g_last;

static void canvas_free(Canvas *canvas) {
    nk_overlay_host_unbind(g_host);
    if (canvas->renderer) SDL_DestroyRenderer(canvas->renderer);
    if (canvas->surface) SDL_DestroySurface(canvas->surface);
    canvas->renderer = NULL;
    canvas->surface = NULL;
}

static bool canvas_make(Canvas *canvas, int w, int h) {
    canvas_free(canvas);
    canvas->surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_RGBA32);
    canvas->renderer = canvas->surface ? SDL_CreateSoftwareRenderer(canvas->surface) : NULL;
    return canvas->renderer != NULL;
}

static uint32_t pixel(const Canvas *canvas, int x, int y) {
    const uint8_t *row = (const uint8_t *)canvas->surface->pixels + (size_t)y * (size_t)canvas->surface->pitch;
    const uint8_t *p = row + (size_t)x * 4u;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* One presenter frame: the base, then the pages over it. */
static bool draw(void) {
    SDL_FillSurfaceRect(g_canvas.surface, NULL,
                        SDL_MapSurfaceRGBA(g_canvas.surface, (Uint8)(BASE_RGBA >> 24),
                                           (Uint8)(BASE_RGBA >> 16), (Uint8)(BASE_RGBA >> 8),
                                           (Uint8)BASE_RGBA));
    if (!nk_overlay_host_draw(g_host, g_canvas.renderer, 1.0f, 1.0f, &g_model, &g_last)) return false;
    SDL_RenderPresent(g_canvas.renderer);
    return true;
}

/* Draws until the host has nothing due, as the presenter's frame loop does. */
static bool settle(void) {
    int i;
    for (i = 0; i < 4; ++i) {
        if (!draw()) return false;
        if (g_last.kind != NK_OVERLAY_ACT_NONE) return true;
        if (!nk_overlay_host_due(g_host, (double)SDL_GetTicksNS() / 1e9)) return true;
    }
    return true;
}

static bool key(SDL_Keycode code) {
    SDL_Event e;
    bool changed;
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_KEY_DOWN;
    e.key.key = code;
    e.key.down = true;
    changed = nk_overlay_host_event(g_host, (const union SDL_Event *)&e);
    e.type = SDL_EVENT_KEY_UP;
    e.key.down = false;
    (void)nk_overlay_host_event(g_host, (const union SDL_Event *)&e);
    return changed;
}

static void test_menu_to_settings_and_back(void) {
    size_t card_pixels = 0;
    int x, y;
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(nk_overlay_host_is_open(g_host), "open: the host is open");
    CHECK(settle(), "open: the menu draws");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE, "open: drawing chooses nothing");
    CHECK(g_model.logical_width == 1280.0f && g_model.logical_height == 800.0f,
          "open: the host fills in the logical size");
    for (y = 0; y < g_canvas.surface->h; y += 4) {
        for (x = 0; x < g_canvas.surface->w; x += 4) {
            if (pixel(&g_canvas, x, y) != BASE_RGBA) ++card_pixels;
        }
    }
    CHECK(card_pixels > 1000u, "open: the pages are drawn over the base");
    CHECK(pixel(&g_canvas, 2, 2) == BASE_RGBA, "open: the corner shows the base");

    /* Down to Settings, then Enter: the settings page. */
    CHECK(key(SDLK_DOWN), "menu: a key press asks for a redraw");
    CHECK(settle(), "menu: draw after down");
    CHECK(key(SDLK_RETURN), "menu: enter");
    CHECK(settle(), "menu: draw after enter");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE, "menu: opening settings is a page change");
    /* Right on the focused scaling row picks the next mode. */
    g_model.scaling = NK_OVERLAY_SCALING_SHARP;
    CHECK(key(SDLK_RIGHT), "settings: right");
    CHECK(draw(), "settings: draw after right");
    CHECK(g_last.kind == NK_OVERLAY_ACT_SET_SCALING && g_last.value == NK_OVERLAY_SCALING_INTEGER,
          "settings: right picks Integer through the real key path");
    /* Esc goes back to the menu, then resumes from it. */
    CHECK(key(SDLK_ESCAPE), "settings: esc");
    CHECK(settle(), "settings: draw after esc");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE, "settings: esc goes back a page, not into the game");
    CHECK(key(SDLK_ESCAPE), "menu: esc");
    CHECK(draw(), "menu: draw after esc");
    CHECK(g_last.kind == NK_OVERLAY_ACT_RESUME, "menu: esc resumes");
    nk_overlay_host_close(g_host);
    CHECK(!nk_overlay_host_is_open(g_host), "close: the host is closed");
    CHECK(!key(SDLK_RETURN), "close: a closed host takes no input");
}

static void test_f1_resumes_and_queue_is_dropped(void) {
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(settle(), "f1: draw");
    CHECK(key(SDLK_F1), "f1: press");
    /* Closed before the frame that would have taken it: the next pause must not see it. */
    nk_overlay_host_close(g_host);
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_USER);
    CHECK(settle(), "f1: the next pause draws");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE, "f1: a press queued before close does not resume the next pause");
    CHECK(key(SDLK_F1), "f1: press again");
    CHECK(draw(), "f1: draw");
    CHECK(g_last.kind == NK_OVERLAY_ACT_RESUME, "f1: resumes from the menu");
    nk_overlay_host_close(g_host);
}

static void test_pad_lost_and_resize(void) {
    nk_overlay_host_open(g_host, NK_OVERLAY_OPEN_PAD_LOST);
    CHECK(settle(), "pad lost: draws");
    CHECK(!g_model.pad_connected, "pad lost: no pad is connected");
    /* Esc keeps the game paused: the menu, never the game. */
    CHECK(key(SDLK_ESCAPE), "pad lost: esc");
    CHECK(settle(), "pad lost: draw after esc");
    CHECK(g_last.kind == NK_OVERLAY_ACT_NONE, "pad lost: back goes to the menu");
    /* The window grows: a new renderer, and the host follows it. */
    CHECK(canvas_make(&g_canvas, 1920, 1200), "resize: a new canvas");
    CHECK(settle(), "resize: draws into the new renderer");
    CHECK(g_model.logical_width == 1920.0f, "resize: the logical size follows");
    CHECK(key(SDLK_ESCAPE), "resize: esc");
    CHECK(draw(), "resize: draw");
    CHECK(g_last.kind == NK_OVERLAY_ACT_RESUME, "resize: esc on the menu resumes");
    nk_overlay_host_close(g_host);
}

int main(int argc, char **argv) {
    char error[256];
    const char *root = argc > 1 ? argv[1] : ".";
    NkUiTtf *probe;
    if (!SDL_Init(0)) {
        printf("SKIP overlay host: SDL_Init failed: %s\n", SDL_GetError());
        return EXIT_SKIP;
    }
    probe = nk_ui_ttf_create(NULL, 0u);
    if (probe == NULL) {
        printf("SKIP overlay host: SDL3_ttf is not usable here: %s\n", nk_ui_ttf_last_error());
        SDL_Quit();
        return EXIT_SKIP;
    }
    nk_ui_ttf_destroy(probe);
    /* A folder without the fonts falls back to the runtime's folder and the working
       directory; the test runs from the repository root, which has them. */
    g_host = nk_overlay_host_create("does-not-exist", error, sizeof(error));
    CHECK(g_host != NULL, "create: a folder without fonts falls back to the working directory");
    nk_overlay_host_destroy(g_host);
    g_host = nk_overlay_host_create(root, error, sizeof(error));
    if (g_host == NULL) {
        fprintf(stderr, "FAIL: create the host from %s: %s\n", root, error);
        SDL_Quit();
        return 1;
    }
    CHECK(canvas_make(&g_canvas, 1280, 800), "a 1280x800 canvas");
    memset(&g_model, 0, sizeof(g_model));
    g_model.title_name = "Overlay Host Fixture";
    g_model.disc_id = "TEST00001";
    test_menu_to_settings_and_back();
    test_f1_resumes_and_queue_is_dropped();
    test_pad_lost_and_resize();
    canvas_free(&g_canvas);
    nk_overlay_host_destroy(g_host);
    SDL_Quit();
    if (failures != 0) {
        fprintf(stderr, "overlay host: %d failure(s)\n", failures);
        return 1;
    }
    printf("overlay host: PASS\n");
    return 0;
}
#endif
