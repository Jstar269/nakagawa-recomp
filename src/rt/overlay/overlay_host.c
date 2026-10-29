/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* The in-game overlay's SDL side in the runtime (#1159, C9c). It is the player's
   nk_ui host (src/player/ui/player_ui_nk.c) cut down to what a paused game needs:
   no router, art or sounds, an overlay-host NkUi (no waves), and pages drawn over
   the image the pause presenter composes. */

#include "overlay_host.h"

#include "nk_ui_input.h"
#include "nk_ui_theme.h"
#include "backend/nk_ui_sdlr.h"

#include <SDL3/SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define OVERLAY_TTF_LIBRARY "SDL3_ttf.dll"
#elif defined(__APPLE__)
#define OVERLAY_TTF_LIBRARY "libSDL3_ttf.0.dylib"
#else
#define OVERLAY_TTF_LIBRARY "libSDL3_ttf.so.0"
#endif

#define OVERLAY_PATH_MAX 1024

struct NkOverlayHost {
    NkUi *ui;
    NkUiTtf *ttf;
    NkUiTextBackend text;
    NkUiSdlr *backend;
    struct SDL_Renderer *bound;   /* the renderer `backend` draws into */
    NkUiSdlInput input;
    NkUiPromptImages prompt_images;
    NkUiViewport viewport;
    NkOverlayState state;
    int page_frames;              /* frames drawn of the page that is up */
};

static void host_error(char *out, size_t size, const char *message) {
    if (out != NULL && size > 0u) snprintf(out, size, "%s", message);
}

static bool join(const char *root, const char *relative, char *out, size_t size) {
    size_t length;
    int written;
    if (root == NULL || root[0] == '\0') return false;
    length = strlen(root);
    written = snprintf(out, size, "%s%s%s", root,
                       root[length - 1u] == '/' || root[length - 1u] == '\\' ? "" : "/",
                       relative);
    return written > 0 && (size_t)written < size;
}

static bool file_exists(const char *path) {
    FILE *file = fopen(path, "rb");
    if (file == NULL) return false;
    fclose(file);
    return true;
}

/* The design system's eight font slots, from the first folder that has the face:
   the player's, the runtime's own, then the working directory. Every face falls
   back to Geist for Latin and to Noto Sans JP for Japanese names, as in the player. */
static bool register_fonts(NkUiTtf *ttf, const char *const *roots, size_t root_count) {
    int font;
    for (font = 0; font < NK_UI_FONT_COUNT; ++font) {
        const char *relative = nk_ui_font_path(font);
        char path[OVERLAY_PATH_MAX];
        bool added = false;
        size_t r;
        for (r = 0; r < root_count && !added; ++r) {
            added = join(roots[r], relative, path, sizeof(path)) && file_exists(path) &&
                    nk_ui_ttf_add_font(ttf, font, path);
        }
        if (!added && !(file_exists(relative) && nk_ui_ttf_add_font(ttf, font, relative))) {
            return false;
        }
    }
    for (font = 0; font < NK_UI_FONT_COUNT; ++font) {
        if (font == NK_UI_FONT_CJK) continue;
        if ((font != NK_UI_FONT_BODY && !nk_ui_ttf_set_fallback(ttf, font, NK_UI_FONT_BODY)) ||
            !nk_ui_ttf_set_fallback(ttf, font, NK_UI_FONT_CJK)) {
            return false;
        }
    }
    return true;
}

NkOverlayHost *nk_overlay_host_create(const char *ui_root, char *error, size_t error_size) {
    NkOverlayHost *host;
    const char *base = SDL_GetBasePath();
    const char *roots[2];
    size_t root_count = 0u;
    char libraries[2][OVERLAY_PATH_MAX];
    const char *library_paths[2];
    size_t library_count = 0u;
    NkUiConfig config;
    if (!nk_ui_sdlr_available() || !nk_ui_ttf_available()) {
        host_error(error, error_size, "this runtime was built without the SDL3_ttf headers");
        return NULL;
    }
    if (ui_root != NULL && ui_root[0] != '\0') roots[root_count++] = ui_root;
    if (base != NULL && base[0] != '\0') roots[root_count++] = base;
    /* SDL3_ttf beside the player (a user may drop it there), beside the runtime, then
       the platform's default search. */
    for (size_t r = 0; r < root_count; ++r) {
        if (join(roots[r], OVERLAY_TTF_LIBRARY, libraries[library_count], OVERLAY_PATH_MAX)) {
            library_paths[library_count] = libraries[library_count];
            ++library_count;
        }
    }
    host = (NkOverlayHost *)calloc(1u, sizeof(*host));
    if (host == NULL) {
        host_error(error, error_size, "out of memory");
        return NULL;
    }
    host->ttf = nk_ui_ttf_create(library_count ? library_paths : NULL, library_count);
    if (host->ttf == NULL) {
        host_error(error, error_size, nk_ui_ttf_last_error());
        nk_overlay_host_destroy(host);
        return NULL;
    }
    if (!register_fonts(host->ttf, roots, root_count)) {
        host_error(error, error_size, "the bundled UI fonts (assets/fonts/ui) were not found");
        nk_overlay_host_destroy(host);
        return NULL;
    }
    host->text = nk_ui_ttf_backend(host->ttf);
    memset(&config, 0, sizeof(config));
    config.host = NK_UI_HOST_OVERLAY;
    config.text = &host->text;
    host->ui = nk_ui_create(&config);
    if (host->ui == NULL) {
        host_error(error, error_size, "the UI layout core could not be created");
        nk_overlay_host_destroy(host);
        return NULL;
    }
    /* Glyphs are an extra: without them the prompts are the geometric forms. */
    (void)nk_ui_prompt_images_load(&host->prompt_images);
    nk_ui_set_prompt_images(host->ui, &host->prompt_images);
    nk_ui_sdl_input_init(&host->input);
    if (error != NULL && error_size > 0u) error[0] = '\0';
    return host;
}

void nk_overlay_host_destroy(NkOverlayHost *host) {
    if (host == NULL) return;
    nk_ui_sdlr_destroy(host->backend);
    nk_ui_destroy(host->ui);
    nk_ui_prompt_images_free(&host->prompt_images);
    nk_ui_ttf_destroy(host->ttf);
    free(host);
}

void nk_overlay_host_open(NkOverlayHost *host, NkOverlayOpenReason reason) {
    int count = 0;
    SDL_JoystickID *pads;
    if (host == NULL) return;
    nk_overlay_open(&host->state, reason);
    host->page_frames = 0;
    /* The pads connected now, each told to the input model the way it would have
       heard of them: their connections went to the game's input while it ran. */
    nk_ui_sdl_input_init(&host->input);
    pads = SDL_GetGamepads(&count);
    for (int i = 0; pads != NULL && i < count; ++i) {
        SDL_Event added;
        NkUiEvent ignored[4];
        memset(&added, 0, sizeof(added));
        added.type = SDL_EVENT_GAMEPAD_ADDED;
        added.gdevice.which = pads[i];
        (void)nk_ui_sdl_input_translate(&host->input, &added, (double)SDL_GetTicksNS() / 1e9,
                                        ignored, sizeof(ignored) / sizeof(ignored[0]));
    }
    SDL_free(pads);
}

void nk_overlay_host_close(NkOverlayHost *host) {
    if (host == NULL) return;
    nk_overlay_close(&host->state);
    host->page_frames = 0;
    /* An empty frame drops what is still queued and the focus it had: the next
       pause starts on its page's default. */
    nk_ui_begin(host->ui, (double)SDL_GetTicksNS() / 1e9);
    (void)nk_ui_end(host->ui);
}

bool nk_overlay_host_is_open(const NkOverlayHost *host) {
    return host != NULL && host->state.open;
}

bool nk_overlay_host_event(NkOverlayHost *host, const SDL_Event *event) {
    NkUiEvent translated[8];
    size_t count;
    if (host == NULL || event == NULL || !host->state.open) return false;
    count = nk_ui_sdl_input_translate(&host->input, event, (double)SDL_GetTicksNS() / 1e9,
                                      translated, sizeof(translated) / sizeof(translated[0]));
    for (size_t i = 0u; i < count; ++i) nk_ui_push_event(host->ui, &translated[i]);
    /* A pad coming or going changes the controller dialog and the prompt set even
       when it brings no UI event. */
    return count > 0u || event->type == SDL_EVENT_GAMEPAD_ADDED ||
           event->type == SDL_EVENT_GAMEPAD_REMOVED;
}

bool nk_overlay_host_due(const NkOverlayHost *host, double now_s) {
    double at = 0.0;
    if (host == NULL || !host->state.open) return false;
    if (host->page_frames < 2) return true;
    return nk_ui_input_next_wake(&host->input.input, &at) && now_s >= at;
}

bool nk_overlay_host_draw(NkOverlayHost *host, SDL_Renderer *renderer, float display_scale,
                          float pixels_per_point, NkOverlayModel *model,
                          NkOverlayAction *action) {
    int pixel_w = 0;
    int pixel_h = 0;
    NkUiScale scale;
    NkUiPrefs prefs;
    NkUiEvent repeats[8];
    size_t repeat_count;
    const NkUiDrawList *list;
    NkOverlayScreen before;
    double now_s;
    if (action != NULL) *action = nk_overlay_action(NK_OVERLAY_ACT_NONE, 0);
    if (host == NULL || renderer == NULL || model == NULL || !host->state.open) return false;
    if (!SDL_GetRenderOutputSize(renderer, &pixel_w, &pixel_h) || pixel_w <= 0 || pixel_h <= 0) {
        return false;
    }
    if (host->bound != renderer) {
        nk_overlay_host_unbind(host);
        host->backend = nk_ui_sdlr_create(renderer, &host->text);
        if (host->backend == NULL) return false;
        host->bound = renderer;
    }
    host->viewport.pixel_w = pixel_w;
    host->viewport.pixel_h = pixel_h;
    host->viewport.display_scale = display_scale > 0.0f ? display_scale : 1.0f;
    host->viewport.ui_scale_pct = 0;
    host->viewport.text_size_pct = 100;
    scale = nk_ui_scale_for_viewport(&host->viewport);
    memset(&prefs, 0, sizeof(prefs));
    prefs.prompt_style = nk_ui_input_prompt_style(&host->input.input);
    nk_ui_set_prefs(host->ui, &prefs);
    nk_ui_sdl_input_set_event_scale(&host->input,
                                    (pixels_per_point > 0.0f ? pixels_per_point
                                                             : host->viewport.display_scale) /
                                        scale.density);
    nk_ui_set_viewport(host->ui, &host->viewport);
    nk_ui_sdlr_set_scale(host->backend, scale.density);
    now_s = (double)SDL_GetTicksNS() / 1e9;
    repeat_count = nk_ui_input_poll(&host->input.input, now_s, repeats,
                                    sizeof(repeats) / sizeof(repeats[0]));
    for (size_t i = 0u; i < repeat_count; ++i) nk_ui_push_event(host->ui, &repeats[i]);
    model->logical_width = scale.logical_w;
    model->logical_height = scale.logical_h;
    model->pad_connected = nk_ui_input_pad_connected(&host->input.input);
    before = host->state.screen;
    nk_ui_begin(host->ui, now_s);
    {
        NkOverlayAction chosen = nk_overlay_frame(host->ui, &host->state, model);
        if (action != NULL) *action = chosen;
    }
    list = nk_ui_end(host->ui);
    /* Focus moves by the rectangles of the frame before, so a page needs two frames
       drawn before it can be navigated, and a focus to give back lands on the next. */
    if (host->state.screen != before) {
        host->page_frames = 0;
    } else if (host->page_frames < 2) {
        ++host->page_frames;
    }
    if (host->state.restore_focus != 0u && host->page_frames >= 2) host->page_frames = 1;
    if (list == NULL) return false;
    return nk_ui_sdlr_render(host->backend, list);
}

void nk_overlay_host_unbind(NkOverlayHost *host) {
    if (host == NULL) return;
    nk_ui_sdlr_destroy(host->backend);
    host->backend = NULL;
    host->bound = NULL;
}

NkUiPadFamily nk_overlay_host_pad_family(const NkOverlayHost *host) {
    const NkUiInput *input;
    NkUiPadFamily family;
    if (host == NULL) return NK_UI_PAD_NEUTRAL;
    input = &host->input.input;
    family = nk_ui_input_last_family(input);
    if (family == NK_UI_PAD_KEYBOARD) {
        /* The keyboard was used last: name the buttons for the pad that is there. */
        family = input->pad_count > 0u ? input->pad_families[0] : NK_UI_PAD_NEUTRAL;
    }
    return family;
}

bool nk_overlay_host_pad_connected(const NkOverlayHost *host) {
    return host != NULL && nk_ui_input_pad_connected(&host->input.input);
}

NkOverlayScreen nk_overlay_host_screen(const NkOverlayHost *host) {
    return host != NULL && host->state.open ? host->state.screen : NK_OVERLAY_SCREEN_MENU;
}

const NkOverlayVramView *nk_overlay_host_vram_view(const NkOverlayHost *host) {
    return host != NULL ? &host->state.vram : NULL;
}
