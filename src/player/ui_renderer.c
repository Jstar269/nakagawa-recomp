/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#include "ui_renderer.h"
#include "ui_clip.h"
#include "iso_reader.h"
#include "nk_platform.h"
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_version.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Color palette */
static const SDL_Color COLOR_BG            = { 12,  15,  18, 255 }; /* #0c0f12 */
static const SDL_Color COLOR_CARD_BG       = { 22,  28,  34, 255 }; /* #161c22 */
static const SDL_Color COLOR_CARD_BORDER   = { 38,  50,  61, 255 }; /* #26323d */
static const SDL_Color COLOR_CARD_HOVER    = { 30,  38,  46, 255 };
static const SDL_Color COLOR_TEXT_WHITE    = { 241, 245, 249, 255 }; /* #f1f5f9 */
static const SDL_Color COLOR_TEXT_MUTED    = { 148, 163, 184, 255 }; /* #94a3b8 */
static const SDL_Color COLOR_TEXT_DIM      = { 100, 116, 139, 255 }; /* #64748b */
static const SDL_Color COLOR_EMERALD       = { 16,  185, 129, 255 }; /* #10b981 */
static const SDL_Color COLOR_EMERALD_HOVER = { 5,   150, 105, 255 };
static const SDL_Color COLOR_LIME          = { 132, 204,  22, 255 }; /* #84cc16 */
static const SDL_Color COLOR_AMBER         = { 245, 158,  11, 255 }; /* #f59e0b */
static const SDL_Color COLOR_RED           = { 239,  68,  68, 255 }; /* #ef4444 */
static const SDL_Color COLOR_BLUE          = { 59,  130, 246, 255 }; /* #3b82f6 */

static void set_draw_color(SDL_Renderer *ren, SDL_Color c) {
    SDL_SetRenderDrawColor(ren, c.r, c.g, c.b, c.a);
}

static void draw_filled_rect(SDL_Renderer *ren, float x, float y, float w, float h, SDL_Color c) {
    set_draw_color(ren, c);
    SDL_FRect r = { x, y, w, h };
    SDL_RenderFillRect(ren, &r);
}

static void draw_rect_outline(SDL_Renderer *ren, float x, float y, float w, float h, SDL_Color c) {
    set_draw_color(ren, c);
    SDL_FRect r = { x, y, w, h };
    SDL_RenderRect(ren, &r);
}

/* --- Runtime typography layer ---
 *
 * Fully legal by construction: no font is bundled, vendored, or
 * redistributed. At startup the renderer tries to load the platform's
 * SDL3_ttf shared library (if the user happens to have it) and open a
 * system UI font already licensed on the user's own machine. If the
 * library, every symbol, or every font file is missing, the classic
 * SDL_RenderDebugText path below draws everything, so the UI works with
 * whatever is thrown at it. Set NK_UI_NO_TTF=1 to force the fallback
 * (e.g. CI machines without the library). Nothing here is linked:
 * SDL_LoadObject/SDL_LoadFunction resolve everything at runtime, so the
 * Makefile gains no dependency and builds never require SDL3_ttf. */

typedef struct TTF_Font TTF_Font; /* opaque; resolved via dlsym, no headers */

typedef bool (SDLCALL *UiTtfInitFn)(void);
typedef void (SDLCALL *UiTtfQuitFn)(void);
typedef TTF_Font *(SDLCALL *UiTtfOpenFontFn)(const char *file, float ptsize);
typedef void (SDLCALL *UiTtfCloseFontFn)(TTF_Font *font);
typedef bool (SDLCALL *UiTtfSetFontSizeFn)(TTF_Font *font, float ptsize);
typedef bool (SDLCALL *UiTtfGetStringSizeFn)(TTF_Font *font, const char *text, size_t length, int *w, int *h);
typedef bool (SDLCALL *UiTtfMeasureStringFn)(TTF_Font *font, const char *text, size_t length, int max_width, int *measured_width, size_t *measured_length);
typedef SDL_Surface *(SDLCALL *UiTtfRenderTextFn)(TTF_Font *font, const char *text, size_t length, SDL_Color fg);

#define UI_FONT_CACHE_ENTRIES 96
#define UI_FONT_CACHE_TEXT 160

typedef struct {
    char text[UI_FONT_CACHE_TEXT];
    int size_bucket;    /* (int)(scale * 10): distinct raster per bucket */
    uint32_t color_key; /* packed RGBA */
    float density;      /* pixel-density multiplier the raster was made for */
    SDL_Texture *tex;
    int w;
    int h;
    uint64_t last_used;
} UiFontCacheEntry;

typedef struct {
    bool attempted;
    bool ready;
    SDL_SharedObject *lib;
    UiTtfInitFn f_init;
    UiTtfQuitFn f_quit;
    UiTtfOpenFontFn f_open;
    UiTtfCloseFontFn f_close;
    UiTtfSetFontSizeFn f_set_size;
    UiTtfGetStringSizeFn f_measure;
    UiTtfMeasureStringFn f_measure_prefix;
    UiTtfRenderTextFn f_render;
    TTF_Font *font;
    int active_bucket;  /* size bucket the font object is currently set to */
    float density;
    UiFontCacheEntry entries[UI_FONT_CACHE_ENTRIES];
    uint64_t tick;
} UiFont;

static UiFont g_font;

static uint32_t ui_color_key(SDL_Color c) {
    return ((uint32_t)c.r << 24) | ((uint32_t)c.g << 16) | ((uint32_t)c.b << 8) | c.a;
}

/* Point size for a DebugText-style scale. 11pt at scale 1 keeps the new
 * proportions close to the old 8px debug glyphs so existing layouts hold. */
static float ui_font_pt_for_scale(float scale, float density) {
    if (density < 1.0f) density = 1.0f;
    if (density > 3.0f) density = 3.0f;
    return 11.0f * scale * density;
}

static bool ui_font_file_exists(const char *path) {
    if (!path || !*path) return false;
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

/* System UI font candidates: narrow per-OS table (a platform backend, not
 * guest semantics). First existing file wins. */
static bool ui_font_find_system_font(char *out_path, size_t out_len) {
#if defined(_WIN32) || defined(_WIN64)
    /* Check user-provided open-source fonts first (%LOCALAPPDATA%\nakagawa\fonts\),
     * followed by Windows system fonts (%WINDIR%\Fonts\).
     * Open-source font guidelines:
     * - Native UI & Setup Wizard: Inter, Roboto Flex, Rubik (SIL OFL)
     * - In-Game HUD: M PLUS Rounded 1c, Rubik, Nunito (SIL OFL)
     * - Japanese Text: Kosugi Maru, Zen Maru Gothic (SIL OFL) */
    static const char *kOpenSourceFiles[] = {
        "Inter-Regular.ttf", "Inter.ttf",
        "RobotoFlex-Regular.ttf", "Roboto-Regular.ttf",
        "Rubik-Regular.ttf", "Rubik.ttf",
        "MPLUSRounded1c-Regular.ttf", "Nunito-Regular.ttf",
        "KosugiMaru-Regular.ttf",
        NULL
    };
    const char *appdata = getenv("LOCALAPPDATA");
    if (appdata && *appdata) {
        for (int i = 0; kOpenSourceFiles[i]; i++) {
            snprintf(out_path, out_len, "%s\\nakagawa\\fonts\\%s", appdata, kOpenSourceFiles[i]);
            if (ui_font_file_exists(out_path)) return true;
        }
    }

    const char *windir = getenv("WINDIR");
    char base[MAX_PATH_LEN];
    if (windir && *windir) {
        snprintf(base, sizeof(base), "%s", windir);
    } else {
        snprintf(base, sizeof(base), "C:\\Windows");
    }
    for (int i = 0; kOpenSourceFiles[i]; i++) {
        snprintf(out_path, out_len, "%s\\Fonts\\%s", base, kOpenSourceFiles[i]);
        if (ui_font_file_exists(out_path)) return true;
    }

    static const char *kFiles[] = { "segoeui.ttf", "tahoma.ttf", "arial.ttf", NULL };
    for (int i = 0; kFiles[i]; i++) {
        snprintf(out_path, out_len, "%s\\Fonts\\%s", base, kFiles[i]);
        if (ui_font_file_exists(out_path)) return true;
    }
#elif defined(__APPLE__)
    static const char *kFiles[] = {
        "/System/Library/Fonts/Supplemental/Arial.ttf",
        "/System/Library/Fonts/Helvetica.ttc",
        "/Library/Fonts/Arial.ttf",
        NULL
    };
    for (int i = 0; kFiles[i]; i++) {
        if (ui_font_file_exists(kFiles[i])) {
            snprintf(out_path, out_len, "%s", kFiles[i]);
            return true;
        }
    }
#else
    static const char *kFiles[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        NULL
    };
    for (int i = 0; kFiles[i]; i++) {
        if (ui_font_file_exists(kFiles[i])) {
            snprintf(out_path, out_len, "%s", kFiles[i]);
            return true;
        }
    }
#endif
    return false;
}

static void *ui_font_sym(const char *name) {
    if (!g_font.lib || !name) return NULL;
    return (void *)SDL_LoadFunction(g_font.lib, name);
}

/* All-or-nothing load: any missing piece disables the whole layer rather
 * than running a half-wired text stack. */
static void ui_font_ensure(void) {
    if (g_font.attempted) return;
    g_font.attempted = true;
    g_font.active_bucket = -1;
    g_font.density = 1.0f;

    const char *force_off = getenv("NK_UI_NO_TTF");
    if (force_off && *force_off) return;

#if defined(_WIN32) || defined(_WIN64)
    static const char *kLibs[] = { "SDL3_ttf.dll", NULL };
#elif defined(__APPLE__)
    static const char *kLibs[] = { "libSDL3_ttf.0.dylib", "libSDL3_ttf.dylib", NULL };
#else
    static const char *kLibs[] = { "libSDL3_ttf.so.0", "libSDL3_ttf.so", NULL };
#endif
    for (int i = 0; kLibs[i]; i++) {
        g_font.lib = SDL_LoadObject(kLibs[i]);
        if (g_font.lib) break;
    }
    if (!g_font.lib) return;

    g_font.f_init = (UiTtfInitFn)ui_font_sym("TTF_Init");
    g_font.f_quit = (UiTtfQuitFn)ui_font_sym("TTF_Quit");
    g_font.f_open = (UiTtfOpenFontFn)ui_font_sym("TTF_OpenFont");
    g_font.f_close = (UiTtfCloseFontFn)ui_font_sym("TTF_CloseFont");
    g_font.f_set_size = (UiTtfSetFontSizeFn)ui_font_sym("TTF_SetFontSize");
    g_font.f_measure = (UiTtfGetStringSizeFn)ui_font_sym("TTF_GetStringSize");
    g_font.f_measure_prefix = (UiTtfMeasureStringFn)ui_font_sym("TTF_MeasureString");
    g_font.f_render = (UiTtfRenderTextFn)ui_font_sym("TTF_RenderText_Blended");
    if (!g_font.f_init || !g_font.f_quit || !g_font.f_open || !g_font.f_close ||
        !g_font.f_set_size || !g_font.f_measure || !g_font.f_measure_prefix || !g_font.f_render) {
        SDL_UnloadObject(g_font.lib);
        g_font.lib = NULL;
        return;
    }
    if (!g_font.f_init()) {
        SDL_UnloadObject(g_font.lib);
        g_font.lib = NULL;
        return;
    }
    char font_path[MAX_PATH_LEN];
    if (!ui_font_find_system_font(font_path, sizeof(font_path))) {
        g_font.f_quit();
        SDL_UnloadObject(g_font.lib);
        g_font.lib = NULL;
        return;
    }
    g_font.font = g_font.f_open(font_path, ui_font_pt_for_scale(1.0f, 1.0f));
    if (!g_font.font) {
        g_font.f_quit();
        SDL_UnloadObject(g_font.lib);
        g_font.lib = NULL;
        return;
    }
    g_font.ready = true;
}

/* --- Per-game ISO texture cache (ICON0.PNG and PIC1.PNG) --- */
typedef struct {
    char iso_path[MAX_PATH_LEN];
    SDL_Texture *icon_tex;
    float icon_w;
    float icon_h;
    bool icon_attempted;
    SDL_Texture *pic1_tex;
    float pic1_w;
    float pic1_h;
    bool pic1_attempted;
} GameTextureCacheEntry;

#define GAME_TEXTURE_CACHE_SIZE 64
static GameTextureCacheEntry s_game_textures[GAME_TEXTURE_CACHE_SIZE];

static GameTextureCacheEntry *ui_get_game_texture_entry(const char *iso_path) {
    if (!iso_path || !*iso_path) return NULL;
    for (int i = 0; i < GAME_TEXTURE_CACHE_SIZE; i++) {
        if (s_game_textures[i].iso_path[0] && strcmp(s_game_textures[i].iso_path, iso_path) == 0) {
            return &s_game_textures[i];
        }
    }
    for (int i = 0; i < GAME_TEXTURE_CACHE_SIZE; i++) {
        if (!s_game_textures[i].iso_path[0]) {
            strncpy(s_game_textures[i].iso_path, iso_path, sizeof(s_game_textures[i].iso_path) - 1);
            s_game_textures[i].iso_path[sizeof(s_game_textures[i].iso_path) - 1] = '\0';
            return &s_game_textures[i];
        }
    }
    if (s_game_textures[0].icon_tex) {
        SDL_DestroyTexture(s_game_textures[0].icon_tex);
        s_game_textures[0].icon_tex = NULL;
    }
    if (s_game_textures[0].pic1_tex) {
        SDL_DestroyTexture(s_game_textures[0].pic1_tex);
        s_game_textures[0].pic1_tex = NULL;
    }
    memset(&s_game_textures[0], 0, sizeof(s_game_textures[0]));
    strncpy(s_game_textures[0].iso_path, iso_path, sizeof(s_game_textures[0].iso_path) - 1);
    s_game_textures[0].iso_path[sizeof(s_game_textures[0].iso_path) - 1] = '\0';
    return &s_game_textures[0];
}

static void ui_game_textures_shutdown(void) {
    for (int i = 0; i < GAME_TEXTURE_CACHE_SIZE; i++) {
        if (s_game_textures[i].icon_tex) {
            SDL_DestroyTexture(s_game_textures[i].icon_tex);
            s_game_textures[i].icon_tex = NULL;
        }
        if (s_game_textures[i].pic1_tex) {
            SDL_DestroyTexture(s_game_textures[i].pic1_tex);
            s_game_textures[i].pic1_tex = NULL;
        }
        s_game_textures[i].iso_path[0] = '\0';
        s_game_textures[i].icon_attempted = false;
        s_game_textures[i].pic1_attempted = false;
    }
}

void ui_font_shutdown(void) {
    ui_game_textures_shutdown();
    if (!g_font.attempted) return;
    for (int i = 0; i < UI_FONT_CACHE_ENTRIES; i++) {
        if (g_font.entries[i].tex) {
            SDL_DestroyTexture(g_font.entries[i].tex);
            g_font.entries[i].tex = NULL;
        }
        g_font.entries[i].text[0] = '\0';
    }
    if (g_font.font && g_font.f_close) {
        g_font.f_close(g_font.font);
        g_font.font = NULL;
    }
    if (g_font.lib) {
        if (g_font.f_quit) g_font.f_quit();
        SDL_UnloadObject(g_font.lib);
        g_font.lib = NULL;
    }
    g_font.ready = false;
}

void ui_font_set_density(float density) {
    if (density >= 1.0f && density <= 3.0f) {
        g_font.density = density;
    }
}

/* Select the raster size for a bucket; cached textures key the bucket, so
 * switching sizes never poisons another scale's glyphs. */
static bool ui_font_begin_bucket(int bucket, float density) {
    if (!g_font.ready) return false;
    if (bucket == g_font.active_bucket && density == g_font.density) return true;
    float scale = (float)bucket / 10.0f;
    if (!g_font.f_set_size(g_font.font, ui_font_pt_for_scale(scale, density))) return false;
    g_font.active_bucket = bucket;
    g_font.density = density;
    return true;
}

static float ui_font_text_width(const char *str, float scale) {
    ui_font_ensure();
    if (!g_font.ready || !str || !*str || scale <= 0.0f) {
        return str ? (float)strlen(str) * 8.0f * scale : 0.0f;
    }
    int bucket = (int)(scale * 10.0f);
    if (!ui_font_begin_bucket(bucket, g_font.density)) {
        return (float)strlen(str) * 8.0f * scale;
    }
    int w = 0;
    int h = 0;
    if (!g_font.f_measure(g_font.font, str, strlen(str), &w, &h) || w <= 0) {
        return (float)strlen(str) * 8.0f * scale;
    }
    return (float)w / g_font.density;
}

/* Raster height of a representative line: drives wrapped-line stepping so
 * TTF lines never collide. Falls back to the debug metric. */
static float ui_font_line_height(float scale) {
    ui_font_ensure();
    if (!g_font.ready || scale <= 0.0f) return 8.0f * scale + 4.0f * scale;
    int bucket = (int)(scale * 10.0f);
    if (!ui_font_begin_bucket(bucket, g_font.density)) return 8.0f * scale + 4.0f * scale;
    int w = 0;
    int h = 0;
    if (!g_font.f_measure(g_font.font, "Ag", 2, &w, &h) || h <= 0) {
        return 8.0f * scale + 4.0f * scale;
    }
    return (float)h / g_font.density + 4.0f * scale;
}

/* Width of the longest prefix of str (in bytes) fitting max_width px. */
static size_t ui_font_fit_prefix(const char *str, float scale, float max_width) {
    ui_font_ensure();
    if (!g_font.ready || !str || scale <= 0.0f || max_width <= 0.0f) {
        size_t fallback = (size_t)(max_width / (8.0f * scale));
        size_t len = strlen(str ? str : "");
        return fallback < len ? fallback : len;
    }
    int bucket = (int)(scale * 10.0f);
    if (!ui_font_begin_bucket(bucket, g_font.density)) {
        size_t fallback = (size_t)(max_width / (8.0f * scale));
        size_t len = strlen(str);
        return fallback < len ? fallback : len;
    }
    int max_px = (int)(max_width * g_font.density);
    int measured_w = 0;
    size_t measured_len = 0;
    if (!g_font.f_measure_prefix(g_font.font, str, strlen(str), max_px, &measured_w, &measured_len)) {
        return 0;
    }
    return measured_len;
}

static UiFontCacheEntry *ui_font_cache_lookup(const char *str, int bucket, uint32_t color_key, float density) {
    UiFontCacheEntry *free_slot = NULL;
    UiFontCacheEntry *oldest = &g_font.entries[0];
    for (int i = 0; i < UI_FONT_CACHE_ENTRIES; i++) {
        UiFontCacheEntry *e = &g_font.entries[i];
        if (!e->tex) {
            if (!free_slot) free_slot = e;
            continue;
        }
        if (e->size_bucket == bucket && e->color_key == color_key && e->density == density &&
            strcmp(e->text, str) == 0) {
            e->last_used = ++g_font.tick;
            return e;
        }
        if (e->last_used < oldest->last_used) oldest = e;
    }
    UiFontCacheEntry *slot = free_slot ? free_slot : oldest;
    if (slot->tex) {
        SDL_DestroyTexture(slot->tex);
        slot->tex = NULL;
    }
    return slot;
}

/* Render one string through the cache. Returns false when the caller must
 * use the DebugText fallback (TTF off, overlong string, or GPU failure). */
static bool ui_font_draw_cached(SDL_Renderer *ren, float x, float y, const char *str, float scale, SDL_Color c) {
    ui_font_ensure();
    if (!g_font.ready || !str || !*str || scale <= 0.0f) return false;
    size_t len = strlen(str);
    if (len >= UI_FONT_CACHE_TEXT) return false;
    int bucket = (int)(scale * 10.0f);
    if (!ui_font_begin_bucket(bucket, g_font.density)) return false;
    uint32_t key = ui_color_key(c);
    UiFontCacheEntry *e = ui_font_cache_lookup(str, bucket, key, g_font.density);
    if (!e->tex) {
        SDL_Surface *surf = g_font.f_render(g_font.font, str, len, c);
        if (!surf) return false;
        SDL_Texture *tex = SDL_CreateTextureFromSurface(ren, surf);
        int sw = surf->w;
        int sh = surf->h;
        SDL_DestroySurface(surf);
        if (!tex || sw <= 0 || sh <= 0) {
            if (tex) SDL_DestroyTexture(tex);
            return false;
        }
        snprintf(e->text, sizeof(e->text), "%s", str);
        e->size_bucket = bucket;
        e->color_key = key;
        e->density = g_font.density;
        e->tex = tex;
        e->w = sw;
        e->h = sh;
        e->last_used = ++g_font.tick;
    }
    float dw = (float)e->w / g_font.density;
    float dh = (float)e->h / g_font.density;
    SDL_FRect dst = { x, y, dw, dh };
    if (!SDL_RenderTexture(ren, e->tex, NULL, &dst)) return false;
    return true;
}

static void draw_text(SDL_Renderer *ren, float x, float y, const char *str, float scale, SDL_Color c) {
    if (!str || !*str) return;
    if (ui_font_draw_cached(ren, x, y, str, scale, c)) return;
    set_draw_color(ren, c);
    SDL_SetRenderScale(ren, scale, scale);
    SDL_RenderDebugText(ren, x / scale, y / scale, str);
    SDL_SetRenderScale(ren, 1.0f, 1.0f);
}

/* Width of a string at scale 1: the single metric every layout helper
 * shares, so TTF and fallback agree on what "fits". */
__attribute__((unused)) static float text_width_at_unit(const char *str) {
    return ui_font_text_width(str ? str : "", 1.0f);
}

/* Measured pixel box of a string at scale (TTF raster, else debug box). */
static void text_size_at_scale(const char *str, float scale, float *out_w, float *out_h) {
    float w = ui_font_text_width(str ? str : "", scale);
    float h = ui_font_line_height(scale) - 4.0f * scale;
    if (h < 8.0f * scale) h = 8.0f * scale;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
}

/* Keep titles on one line with visible truncation. Under TTF the prefix is
 * measured in pixels (proportional glyphs have no fixed char budget). */
static void draw_text_ellipsized(SDL_Renderer *ren, float x, float y,
                                 const char *str, float scale, float max_width,
                                 SDL_Color c) {
    if (!str || !*str || scale <= 0.0f || max_width <= 0.0f) return;

    float full_w = ui_font_text_width(str, scale);
    if (full_w <= max_width) {
        draw_text(ren, x, y, str, scale, c);
        return;
    }

    float ell_w = ui_font_text_width("...", scale);
    if (max_width <= ell_w + 1.0f) return;
    size_t prefix_bytes = ui_font_fit_prefix(str, scale, max_width - ell_w);
    /* Never split a UTF-8 sequence: back up over continuation bytes. */
    while (prefix_bytes > 0 && ((unsigned char)str[prefix_bytes] & 0xC0) == 0x80) {
        prefix_bytes--;
    }
    /* Never split a word when a clean break exists in the fitting prefix. */
    if (prefix_bytes > 12) {
        size_t back = prefix_bytes;
        while (back > prefix_bytes - 24 && back > 0 && str[back - 1] != ' ') back--;
        if (back > 0 && str[back - 1] == ' ') prefix_bytes = back - 1;
    }
    if (prefix_bytes >= NK_MAX_TITLE_LEN - 4) prefix_bytes = NK_MAX_TITLE_LEN - 4;
    char clipped[NK_MAX_TITLE_LEN];
    memcpy(clipped, str, prefix_bytes);
    memcpy(clipped + prefix_bytes, "...", 3);
    clipped[prefix_bytes + 3] = '\0';
    draw_text(ren, x, y, clipped, scale, c);
}

/* Center a fixed-width card, never letting it run off either edge. */
static float centered_card_x(float window_w, float card_w) {
    float x = window_w * 0.5f - card_w * 0.5f;
    if (x < 16.0f) x = 16.0f;
    return x;
}

/* Dialog width that fits narrow windows instead of clipping. */
static float dialog_card_w(float window_w, float desired_w) {
    float w = desired_w;
    if (w > window_w - 32.0f) w = window_w - 32.0f;
    if (w < 320.0f) w = 320.0f;
    if (w > window_w) w = window_w;
    return w;
}

static bool is_point_in_rect(float px, float py, float rx, float ry, float rw, float rh) {
    return px >= rx && px <= (rx + rw) && py >= ry && py <= (ry + rh);
}

/* --- Rounded geometry (pure SDL3, no assets) --- */

static float minf(float a, float b) {
    return a < b ? a : b;
}

/* Quarter-arc unit table (cos, sin) at 11.25-degree steps. Precomputed so
 * rounded geometry needs no libm: the player link line carries no -lm and
 * must keep none. */
static const float kArcPts[9][2] = {
    { 1.0f, 0.0f },
    { 0.980785f, 0.195090f },
    { 0.923880f, 0.382683f },
    { 0.831470f, 0.555570f },
    { 0.707107f, 0.707107f },
    { 0.555570f, 0.831470f },
    { 0.382683f, 0.923880f },
    { 0.195090f, 0.980785f },
    { 0.0f, 1.0f }
};

/* Corner arc point: which 0=TL, 1=TR, 2=BR, 3=BL; step 0..8 along the arc. */
static void rounded_arc_point(int which, int step, float cx, float cy, float r,
                              float *out_x, float *out_y) {
    float ax = kArcPts[step][0];
    float ay = kArcPts[step][1];
    float sx = 1.0f;
    float sy = 1.0f;
    if (which == 0) { sx = -ax; sy = -ay; }
    else if (which == 1) { sx = ay; sy = -ax; }
    else if (which == 2) { sx = ax; sy = ay; }
    else { sx = -ay; sy = ax; }
    *out_x = cx + r * sx;
    *out_y = cy + r * sy;
}

/* One 90-degree arc fan corner for rounded fills. */
static int rounded_corner_fill(SDL_Vertex *verts, int at, float cx, float cy, float r,
                               int which, SDL_Color c) {
    SDL_FColor fc = { (float)c.r / 255.0f, (float)c.g / 255.0f, (float)c.b / 255.0f, (float)c.a / 255.0f };
    verts[at].position.x = cx;
    verts[at].position.y = cy;
    verts[at].color = fc;
    verts[at].tex_coord.x = 0.0f;
    verts[at].tex_coord.y = 0.0f;
    for (int i = 0; i <= 8; i++) {
        float px;
        float py;
        rounded_arc_point(which, i, cx, cy, r, &px, &py);
        verts[at + 1 + i].position.x = px;
        verts[at + 1 + i].position.y = py;
        verts[at + 1 + i].color = fc;
        verts[at + 1 + i].tex_coord.x = 0.0f;
        verts[at + 1 + i].tex_coord.y = 0.0f;
    }
    return at + 10;
}

static void draw_rounded_fill(SDL_Renderer *ren, float x, float y, float w, float h, float r, SDL_Color c) {
    if (w <= 0.0f || h <= 0.0f) return;
    r = minf(r, minf(w, h) * 0.5f);
    if (r <= 0.5f) {
        draw_filled_rect(ren, x, y, w, h, c);
        return;
    }
    draw_filled_rect(ren, x + r, y, w - 2.0f * r, h, c);
    draw_filled_rect(ren, x, y + r, w, h - 2.0f * r, c);
    SDL_Vertex verts[4 * 10];
    int at = 0;
    at = rounded_corner_fill(verts, at, x + r, y + r, r, 0, c);
    at = rounded_corner_fill(verts, at, x + w - r, y + r, r, 1, c);
    at = rounded_corner_fill(verts, at, x + w - r, y + h - r, r, 2, c);
    at = rounded_corner_fill(verts, at, x + r, y + h - r, r, 3, c);
    /* Triangle fan per corner needs an index list: 8 triangles each. */
    int indices[4 * 8 * 3];
    int ii = 0;
    for (int corner = 0; corner < 4; corner++) {
        int base = corner * 10;
        for (int i = 0; i < 8; i++) {
            indices[ii++] = base;
            indices[ii++] = base + 1 + i;
            indices[ii++] = base + 2 + i;
        }
    }
    SDL_RenderGeometry(ren, NULL, verts, at, indices, ii);
}

/* Rounded outline from straight edges plus arc polylines. */
static void draw_rounded_outline(SDL_Renderer *ren, float x, float y, float w, float h, float r, SDL_Color c) {
    if (w <= 0.0f || h <= 0.0f) return;
    r = minf(r, minf(w, h) * 0.5f);
    if (r <= 1.0f) {
        draw_rect_outline(ren, x, y, w, h, c);
        return;
    }
    set_draw_color(ren, c);
    SDL_RenderLine(ren, x + r, y, x + w - r, y);
    SDL_RenderLine(ren, x + r, y + h, x + w - r, y + h);
    SDL_RenderLine(ren, x, y + r, x, y + h - r);
    SDL_RenderLine(ren, x + w, y + r, x + w, y + h - r);
    float centers[4][2] = { { x + r, y + r }, { x + w - r, y + r }, { x + w - r, y + h - r }, { x + r, y + h - r } };
    for (int corner = 0; corner < 4; corner++) {
        SDL_FPoint pts[9];
        for (int i = 0; i <= 8; i++) {
            float px;
            float py;
            rounded_arc_point(corner, i, centers[corner][0], centers[corner][1], r, &px, &py);
            pts[i].x = px;
            pts[i].y = py;
        }
        SDL_RenderLines(ren, pts, 9);
    }
}

/* Soft drop shadow: translucent offset silhouette under cards/buttons. */
static void draw_shadow(SDL_Renderer *ren, float x, float y, float w, float h, float r) {
    SDL_Color shadow = { 0, 0, 0, 55 };
    draw_rounded_fill(ren, x, y + 4.0f, w, h, r, shadow);
}

/* --- Title monograms (generated at runtime, zero assets) ---
 * A hue derived from the disc ID plus the title's initials. Nothing is
 * loaded, downloaded, or redistributed, so there is no artwork to license. */

static float ui_hue_for_disc_id(const char *disc_id) {
    if (!disc_id || !*disc_id) return 160.0f;
    unsigned long hash = 5381;
    for (const unsigned char *p = (const unsigned char *)disc_id; *p; p++) {
        hash = hash * 33 + *p;
    }
    return (float)(hash % 360);
}

static void ui_hsl_to_rgb(float h, float s, float l, Uint8 *r, Uint8 *g, Uint8 *b) {
    /* Branch-form HSL conversion: no libm needed (fmod/fabs avoided). */
    while (h < 0.0f) h += 360.0f;
    while (h >= 360.0f) h -= 360.0f;
    if (s < 0.0f) s = 0.0f;
    if (s > 1.0f) s = 1.0f;
    if (l < 0.0f) l = 0.0f;
    if (l > 1.0f) l = 1.0f;
    float c = (1.0f - (l <= 0.5f ? (1.0f - 2.0f * l) : (2.0f * l - 1.0f))) * s;
    float hp = h / 60.0f;
    float hpm2 = hp - ((int)(hp / 2.0f)) * 2.0f; /* hp in [0,6): safe fmod */
    float x = c * (1.0f - (hpm2 <= 1.0f ? (1.0f - hpm2) : (hpm2 - 1.0f)));
    float r1 = 0.0f;
    float g1 = 0.0f;
    float b1 = 0.0f;
    if (hp < 1.0f) { r1 = c; g1 = x; }
    else if (hp < 2.0f) { r1 = x; g1 = c; }
    else if (hp < 3.0f) { g1 = c; b1 = x; }
    else if (hp < 4.0f) { g1 = x; b1 = c; }
    else if (hp < 5.0f) { r1 = x; b1 = c; }
    else { r1 = c; b1 = x; }
    float m = l - c * 0.5f;
    *r = (Uint8)((r1 + m) * 255.0f);
    *g = (Uint8)((g1 + m) * 255.0f);
    *b = (Uint8)((b1 + m) * 255.0f);
}

/* Up to two ASCII initials from the title's first words; "?" when none. */
static void ui_initials_for_title(const char *title, char *out, size_t out_len) {
    if (!out || out_len == 0) return;
    out[0] = '\0';
    if (!title) return;
    size_t n = 0;
    bool word_start = true;
    for (const char *p = title; *p && n + 1 < out_len && n < 2; p++) {
        unsigned char ch = (unsigned char)*p;
        bool is_alnum = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
        if (word_start && is_alnum) {
            out[n++] = (char)((ch >= 'a' && ch <= 'z') ? ch - 32 : ch);
            word_start = false;
        } else if (ch == ' ' || ch == '\t' || ch == '-' || ch == '_') {
            word_start = true;
        }
    }
    out[n] = '\0';
    if (n == 0) {
        snprintf(out, out_len, "?");
    }
}

static void draw_monogram(SDL_Renderer *ren, float x, float y, float size,
                          const char *disc_id, const char *title) {
    if (size < 28.0f) return;
    float hue = ui_hue_for_disc_id(disc_id);
    Uint8 br;
    Uint8 bg;
    Uint8 bb;
    ui_hsl_to_rgb(hue, 0.45f, 0.20f, &br, &bg, &bb);
    Uint8 rr;
    Uint8 rg;
    Uint8 rb;
    ui_hsl_to_rgb(hue, 0.65f, 0.50f, &rr, &rg, &rb);
    draw_rounded_fill(ren, x, y + 2.0f, size, size, 9.0f, (SDL_Color){ 0, 0, 0, 50 });
    draw_rounded_fill(ren, x, y, size, size, 9.0f, (SDL_Color){ br, bg, bb, 255 });
    draw_rounded_outline(ren, x, y, size, size, 9.0f, (SDL_Color){ rr, rg, rb, 255 });
    char initials[4];
    ui_initials_for_title(title, initials, sizeof(initials));
    float tscale = size / 34.0f;
    if (tscale < 0.9f) tscale = 0.9f;
    if (tscale > 2.0f) tscale = 2.0f;
    float tw = ui_font_text_width(initials, tscale);
    float th = ui_font_line_height(tscale) - 4.0f * tscale;
    if (th < 1.0f) th = 8.0f * tscale;
    draw_text(ren, x + (size - tw) * 0.5f, y + (size - th) * 0.5f, initials, tscale,
              (SDL_Color){ 241, 245, 249, 255 });
}

static void ui_load_pic1_if_needed(SDL_Renderer *ren, GameTextureCacheEntry *entry, const char *iso_path) {
    if (!entry || entry->pic1_attempted || !iso_path || !*iso_path) return;
    entry->pic1_attempted = true;
    uint8_t *bytes = NULL;
    size_t size = 0;
    uint32_t w = 0, h = 0;
    NkIconStatus st = nk_iso_read_image_entry(iso_path, "PSP_GAME/PIC1.PNG",
                                              &bytes, &size, &w, &h);
    if (st == NK_ICON_OK && bytes && size > 0) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
        SDL_IOStream *io = SDL_IOFromConstMem(bytes, size);
        if (io) {
            SDL_Surface *surf = SDL_LoadPNG_IO(io, true);
            if (surf) {
                entry->pic1_tex = SDL_CreateTextureFromSurface(ren, surf);
                if (entry->pic1_tex) {
                    SDL_GetTextureSize(entry->pic1_tex, &entry->pic1_w, &entry->pic1_h);
                }
                SDL_DestroySurface(surf);
            }
        }
#else
        (void)ren;
#endif
        free(bytes);
    }
}

static void draw_game_icon(SDL_Renderer *ren, float x, float y, float max_w, float max_h,
                           const GameRecord *game) {
    GameTextureCacheEntry *entry = NULL;
    if (game && game->iso_path[0]) {
        entry = ui_get_game_texture_entry(game->iso_path);
        if (entry && !entry->icon_attempted) {
            entry->icon_attempted = true;
            uint8_t *bytes = NULL;
            size_t size = 0;
            uint32_t w = 0, h = 0;
            NkIconStatus st = nk_iso_read_image_entry(game->iso_path, "PSP_GAME/ICON0.PNG",
                                                      &bytes, &size, &w, &h);
            if (st == NK_ICON_OK && bytes && size > 0) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
                SDL_IOStream *io = SDL_IOFromConstMem(bytes, size);
                if (io) {
                    SDL_Surface *surf = SDL_LoadPNG_IO(io, true);
                    if (surf) {
                        entry->icon_tex = SDL_CreateTextureFromSurface(ren, surf);
                        if (entry->icon_tex) {
                            SDL_GetTextureSize(entry->icon_tex, &entry->icon_w, &entry->icon_h);
                        }
                        SDL_DestroySurface(surf);
                    }
                }
#endif
                free(bytes);
            }
        }
    }

    if (entry && entry->icon_tex && entry->icon_w > 0.0f && entry->icon_h > 0.0f) {
        float aspect = entry->icon_w / entry->icon_h;
        float fit_w = max_w;
        float fit_h = fit_w / aspect;
        if (fit_h > max_h) {
            fit_h = max_h;
            fit_w = fit_h * aspect;
        }
        float offset_x = x + (max_w - fit_w) * 0.5f;
        float offset_y = y + (max_h - fit_h) * 0.5f;

        draw_shadow(ren, offset_x, offset_y, fit_w, fit_h, 4.0f);
        draw_rounded_fill(ren, offset_x, offset_y, fit_w, fit_h, 4.0f, (SDL_Color){ 0, 0, 0, 180 });
        SDL_FRect dst = { offset_x, offset_y, fit_w, fit_h };
        SDL_RenderTexture(ren, entry->icon_tex, NULL, &dst);
        draw_rounded_outline(ren, offset_x, offset_y, fit_w, fit_h, 4.0f, COLOR_CARD_BORDER);
        return;
    }

    float mono_size = max_h;
    if (mono_size > max_w) mono_size = max_w;
    draw_monogram(ren, x + (max_w - mono_size) * 0.5f, y + (max_h - mono_size) * 0.5f,
                  mono_size, game ? game->disc_id : NULL, game ? game->title_name : NULL);
}

/* Per-frame motion preference, set by ui_render_frame from settings. */
static bool g_reduce_motion;

/* Focusable button. Mouse click always wins; keyboard/gamepad activation
 * (Enter/Space/SOUTH) triggers the focused button. The focus ring is a
 * second outline so hover and focus stay visually distinct. */
static bool draw_button_focused(SDL_Renderer *ren, float x, float y, float w, float h,
                                const char *label, bool is_accent, const UiInput *in,
                                bool focused) {
    bool hovered = in ? is_point_in_rect((float)in->mouse_x, (float)in->mouse_y, x, y, w, h) : false;
    bool activated = in && in->activate_pressed && focused;
    SDL_Color bg = is_accent ? (hovered ? COLOR_EMERALD_HOVER : COLOR_EMERALD) : (hovered ? COLOR_CARD_HOVER : COLOR_CARD_BG);
    SDL_Color border = is_accent ? COLOR_EMERALD : COLOR_CARD_BORDER;
    SDL_Color text = is_accent ? (SDL_Color){10, 25, 20, 255} : COLOR_TEXT_WHITE;

    draw_shadow(ren, x, y, w, h, 7.0f);
    draw_rounded_fill(ren, x, y, w, h, 7.0f, bg);
    draw_rounded_outline(ren, x, y, w, h, 7.0f, border);
    if (focused) {
        float pulse = 1.0f;
        if (!g_reduce_motion) {
            uint64_t t = SDL_GetTicks() / 350;
            pulse = (t % 2 == 0) ? 1.0f : 0.55f;
        }
        SDL_Color ring = COLOR_LIME;
        ring.a = (Uint8)(255.0f * pulse);
        draw_rounded_outline(ren, x - 2.0f, y - 2.0f, w + 4.0f, h + 4.0f, 9.0f, ring);
    }
    if (activated) {
        draw_filled_rect(ren, x + 2.0f, y + 2.0f, w - 4.0f, 2.0f, COLOR_LIME);
    }

    /* Fit the label with real text metrics so proportional fonts center. */
    float base_len = ui_font_text_width(label, 1.0f);
    if (base_len < 1.0f) base_len = 1.0f;
    float max_scale = 1.3f;
    float avail_w = w - 20.0f;
    float scale = avail_w / base_len;
    if (scale > max_scale) scale = max_scale;
    if (scale < 0.8f) scale = 0.8f;

    float text_len = 0.0f;
    float text_h = 0.0f;
    text_size_at_scale(label, scale, &text_len, &text_h);
    float tx = x + (w - text_len) * 0.5f;
    float ty = y + (h - text_h) * 0.5f;
    draw_text(ren, tx, ty, label, scale, text);

    if (hovered && in && in->mouse_clicked) return true;
    return activated;
}

static bool draw_button(SDL_Renderer *ren, float x, float y, float w, float h, const char *label, bool is_accent, const UiInput *in) {
    return draw_button_focused(ren, x, y, w, h, label, is_accent, in, false);
}

static float draw_issue_links(SDL_Renderer *ren, float x, float y,
                              const unsigned int *issues, size_t issue_count,
                              const UiInput *in) {
    for (size_t i = 0; issues && i < issue_count; i++) {
        char label[16];
        char url[128];
        snprintf(label, sizeof(label), "#%u", issues[i]);
        float width = 48.0f;
        if (draw_button(ren, x, y, width, 22.0f, label, false, in)) {
            snprintf(url, sizeof(url),
                     "https://github.com/Jstar269/nakagawa-recomp/issues/%u",
                     issues[i]);
            (void)SDL_OpenURL(url);
        }
        x += width + 6.0f;
    }
    return x;
}

/* Non-interactive status pill drawn where a button could be misread as one.
 * Uses the same geometry so cards keep alignment, but never hovers and is
 * never a focus stop. */
static void draw_status_pill(SDL_Renderer *ren, float x, float y, float w, float h, const char *label) {
    draw_rounded_fill(ren, x, y, w, h, 7.0f, (SDL_Color){ 20, 26, 32, 255 });
    draw_rounded_outline(ren, x, y, w, h, 7.0f, COLOR_CARD_BORDER);
    float base_len = ui_font_text_width(label, 1.0f);
    if (base_len < 1.0f) base_len = 1.0f;
    float scale = (w - 20.0f) / base_len;
    if (scale > 1.1f) scale = 1.1f;
    if (scale < 0.8f) scale = 0.8f;
    float text_len = 0.0f;
    float text_h = 0.0f;
    text_size_at_scale(label, scale, &text_len, &text_h);
    draw_text(ren, x + (w - text_len) * 0.5f, y + (h - text_h) * 0.5f, label, scale, COLOR_TEXT_DIM);
}

/* Word-wrap with real text metrics. Splits on '\n' first, then on spaces;
 * overlong words are hard-cut on UTF-8 boundaries. Returns the y of the
 * line after the last one drawn. */
static float draw_text_wrapped(SDL_Renderer *ren, float x, float y, float max_w,
                               const char *str, float scale, SDL_Color c, int max_lines) {
    if (!str || !*str || scale <= 0.0f || max_w <= 0.0f || max_lines <= 0) return y;
    float line_h = ui_font_line_height(scale);

    int line_no = 0;
    const char *cursor = str;
    char line_buf[512];
    while (*cursor && line_no < max_lines) {
        /* Explicit line break: emit an empty line and continue. */
        if (*cursor == '\n') {
            y += line_h;
            line_no++;
            cursor++;
            continue;
        }
        /* Find the next explicit break to bound this paragraph. */
        const char *para_end = strchr(cursor, '\n');
        size_t para_len = para_end ? (size_t)(para_end - cursor) : strlen(cursor);
        size_t offset = 0;
        while (offset < para_len && line_no < max_lines) {
            const char *seg = cursor + offset;
            size_t remaining = para_len - offset;
            /* Width-based chunk: longest fitting byte prefix of the rest. */
            size_t take = ui_font_fit_prefix(seg, scale, max_w);
            if (take > remaining) take = remaining;
            if (take == 0) take = 1;
            if (take >= sizeof(line_buf)) take = sizeof(line_buf) - 1;
            bool last_line = (line_no + 1 >= max_lines);
            bool more_after = (offset + take) < para_len || (para_end && *(para_end + 1));
            /* Prefer a space break when the line would otherwise split. */
            if (take < remaining && !last_line) {
                size_t back = take;
                while (back > 0 && seg[back] != ' ' && seg[back] != '\t') back--;
                if (back > take / 3) take = back;
            }
            /* Never split a UTF-8 sequence. */
            while (take > 1 && ((unsigned char)seg[take] & 0xC0) == 0x80) take--;
            memcpy(line_buf, cursor + offset, take);
            line_buf[take] = '\0';
            /* Trim a leading space carried from the break. */
            const char *emit = line_buf;
            if (*emit == ' ' || *emit == '\t') emit++;
            if (last_line && more_after) {
                size_t emit_len = strlen(emit);
                if (emit_len > 3) {
                    memcpy(line_buf + (emit - line_buf) + emit_len - 3, "...", 3);
                }
                draw_text(ren, x, y, emit, scale, c);
                y += line_h;
                line_no++;
                break;
            }
            draw_text(ren, x, y, emit, scale, c);
            y += line_h;
            line_no++;
            offset += take;
            while (offset < para_len && (cursor[offset] == ' ' || cursor[offset] == '\t')) offset++;
        }
        cursor += para_len;
        if (*cursor == '\n') cursor++;
    }
    return y;
}

static void draw_badge(SDL_Renderer *ren, float x, float y, const char *label, SDL_Color badge_color) {
    float len = ui_font_text_width(label, 1.0f) + 16.0f;
    SDL_Color bg = { (Uint8)(badge_color.r / 4), (Uint8)(badge_color.g / 4), (Uint8)(badge_color.b / 4), 255 };
    draw_rounded_fill(ren, x, y, len, 24.0f, 12.0f, bg);
    draw_rounded_outline(ren, x, y, len, 24.0f, 12.0f, badge_color);
    float tw = 0.0f;
    float th = 0.0f;
    text_size_at_scale(label, 1.0f, &tw, &th);
    draw_text(ren, x + (len - tw) * 0.5f, y + (24.0f - th) * 0.5f, label, 1.0f, badge_color);
}

static void draw_progress_bar(SDL_Renderer *ren, float x, float y, float w, float h, float pct) {
    if (pct < 0.0f) pct = 0.0f;
    if (pct > 100.0f) pct = 100.0f;
    draw_rounded_fill(ren, x, y, w, h, h * 0.5f, (SDL_Color){ 20, 26, 32, 255 });
    draw_rounded_outline(ren, x, y, w, h, h * 0.5f, COLOR_CARD_BORDER);
    if (pct > 0.0f) {
        float fill_w = (w - 4.0f) * (pct / 100.0f);
        if (fill_w > h) {
            draw_rounded_fill(ren, x + 2.0f, y + 2.0f, fill_w, h - 4.0f, (h - 4.0f) * 0.5f, COLOR_EMERALD);
        } else if (fill_w > 0.0f) {
            draw_filled_rect(ren, x + 2.0f, y + 2.0f, fill_w, h - 4.0f, COLOR_EMERALD);
        }
    }
}

/* Indeterminate working indicator: an amber segment sweeping the track.
 * It intentionally claims no percentage or completion state. Frozen
 * centered when reduced motion is on. */
static void draw_indeterminate_bar(SDL_Renderer *ren, float x, float y, float w, float h, bool reduce_motion) {
    const float seg_w = w * 0.3f;
    float pos = (w - seg_w) * 0.5f;
    if (!reduce_motion) {
        const float period_ms = 2400.0f;
        float t = (float)(SDL_GetTicks() % (uint64_t)period_ms) / period_ms; /* 0..1 */
        float travel = w - seg_w;
        /* Triangle wave: sweep right, then back. */
        pos = (t < 0.5f) ? (t * 2.0f * travel) : ((1.0f - t) * 2.0f * travel);
    }

    draw_rounded_fill(ren, x, y, w, h, h * 0.5f, (SDL_Color){ 20, 26, 32, 255 });
    draw_rounded_outline(ren, x, y, w, h, h * 0.5f, COLOR_CARD_BORDER);
    draw_rounded_fill(ren, x + pos, y + 2.0f, seg_w, h - 4.0f, (h - 4.0f) * 0.5f, COLOR_AMBER);
}

/* Format a byte count as a human-readable size string. */
static void format_size(char *buf, size_t buf_len, uint64_t bytes) {
    if (bytes >= (1024ULL * 1024 * 1024)) {
        snprintf(buf, buf_len, "%.2f GB", (double)bytes / (1024.0 * 1024 * 1024));
    } else if (bytes >= (1024ULL * 1024)) {
        snprintf(buf, buf_len, "%.2f MB", (double)bytes / (1024.0 * 1024));
    } else {
        snprintf(buf, buf_len, "%llu bytes", (unsigned long long)bytes);
    }
}

/* Human-readable label for a game support status. */
static const char *status_label(NkGameSupportStatus status) {
    switch (status) {
        case NK_STATUS_UNIDENTIFIED:      return "UNIDENTIFIED";
        case NK_STATUS_IDENTIFIED:        return "IDENTIFIED";
        case NK_STATUS_SUPPORTED_PREPARATION: return "SUPPORTED";
        case NK_STATUS_PREPARED:          return "PREPARED";
        case NK_STATUS_BOOTS:             return "BOOTS";
        case NK_STATUS_PLAYABLE:          return "PLAYABLE";
        case NK_STATUS_VERIFIED:          return "CATALOG MATCH";
        default:                          return "UNKNOWN STATUS";
    }
}

/* Human-readable label for the configured internal resolution scale. */
static const char *resolution_label(int scale) {
    switch (scale) {
        case 1: return "1x Native (480x272)";
        case 2: return "2x Vita (960x544)";
        case 3: return "3x Scale (1440x816)";
        case 4: return "4x Scale (1920x1088)";
        case 8: return "8x Scale (3840x2176)";
        default: return "Custom Scale";
    }
}

/* Human-readable label for the configured frame rate cap. */
static const char *fps_label(int cap) {
    switch (cap) {
        case 30: return "30 FPS CAP";
        case 60: return "60 FPS CAP";
        case 0:  return "UNCAPPED TARGET";
        default: return "CADENCE CAP";
    }
}

/* --- Topbar Header --- */
static void render_topbar(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    draw_filled_rect(ren, 0, 0, w, 64.0f, (SDL_Color){ 16, 21, 26, 255 });
    draw_rect_outline(ren, 0, 63.0f, w, 1.0f, COLOR_CARD_BORDER);

    /* Logo Dot */
    draw_rounded_fill(ren, 24.0f, 22.0f, 18.0f, 18.0f, 5.0f, COLOR_LIME);
    draw_text(ren, 52.0f, 16.0f, "NAKAGAWA RECOMP", 1.8f, COLOR_TEXT_WHITE);
    if (w >= 700.0f) {
        draw_text(ren, 52.0f, 40.0f, "PSP RECOMPILATION PLAYER", 1.0f, COLOR_TEXT_MUTED);
    }

    /* Mode indicator: hidden on narrow windows so it can never sit under
     * the settings button. */
    if (w >= 900.0f) {
        draw_badge(ren, 320.0f, 20.0f, "PLAYER MODE", COLOR_EMERALD);
    }

    if (app->is_game_running && w >= 1100.0f) {
        char pid_str[64];
        snprintf(pid_str, sizeof(pid_str), "GAME ACTIVE (PID %d)", app->launch_session.process.process_id);
        draw_badge(ren, 460.0f, 20.0f, pid_str, COLOR_LIME);
    }

    /* Controller badge: full label on wide windows, compact state dot
     * below that, hidden on the narrowest layouts. */
    if (w >= 1000.0f) {
        if (app->settings.controller_connected) {
            draw_badge(ren, w - 320.0f, 20.0f, "GAMEPAD CONNECTED", COLOR_LIME);
        } else {
            draw_badge(ren, w - 320.0f, 20.0f, "KEYBOARD READY", COLOR_TEXT_DIM);
        }
    } else if (w >= 760.0f) {
        if (app->settings.controller_connected) {
            draw_badge(ren, w - 300.0f, 20.0f, "PAD OK", COLOR_LIME);
        } else {
            draw_badge(ren, w - 300.0f, 20.0f, "KEYBOARD", COLOR_TEXT_DIM);
        }
    }

    /* Settings Button. Mouse-driven; keyboard/gamepad users press S/START
     * (see the footer hints) so the topbar never needs a focus stop. */
    if (draw_button(ren, w - 140.0f, 16.0f, 116.0f, 32.0f, "SETTINGS", false, in)) {
        if (app->active_view == VIEW_SETTINGS) {
            player_app_set_view(app, VIEW_LIBRARY);
        } else {
            player_app_set_view(app, VIEW_SETTINGS);
        }
    }
}

/* One-line keyboard/gamepad hint footer. Truncated to fit; never wraps. */
static void render_footer_hints(SDL_Renderer *ren, PlayerApp *app) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    if (h < 560.0f) return;
    const char *hints = "O: Add ISO · Arrows: Select · Tab: Focus · Enter: Activate · S: Settings · ESC: Back";
    if (w < 900.0f) {
        hints = "O: Add · Arrows: Select · Tab: Focus · Enter: OK · S: Settings";
    }
    if (w < 640.0f) {
        hints = "O: Add · Tab: Focus · Enter: OK";
    }
    float max_w = w - 64.0f;
    if (max_w <= 0.0f) return;
    size_t fit = ui_font_fit_prefix(hints, 1.0f, max_w);
    size_t len = strlen(hints);
    if (fit >= len) {
        draw_text(ren, 32.0f, h - 24.0f, hints, 1.0f, COLOR_TEXT_DIM);
        return;
    }
    while (fit > 0 && ((unsigned char)hints[fit] & 0xC0) == 0x80) fit--;
    char clipped[160];
    if (fit > sizeof(clipped) - 4) fit = sizeof(clipped) - 4;
    if (fit == 0) return;
    memcpy(clipped, hints, fit);
    memcpy(clipped + fit, "...", 3);
    clipped[fit + 3] = '\0';
    draw_text(ren, 32.0f, h - 24.0f, clipped, 1.0f, COLOR_TEXT_DIM);
}

/* --- View: Empty Library --- */
static void render_empty_library(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float cy = h * 0.5f;

    float card_w = dialog_card_w(w, 640.0f);
    float card_h = 360.0f;
    float card_x = centered_card_x(w, card_w);
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;
    if (card_y + card_h > h - 40.0f && h > 480.0f) card_y = h - 40.0f - card_h;

    draw_shadow(ren, card_x, card_y, card_w, card_h, 10.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 32.0f, "PSP GAME LIBRARY", COLOR_BLUE);
    draw_text(ren, card_x + 32.0f, card_y + 76.0f, "Your Games, Recompiled for PC", 2.2f, COLOR_TEXT_WHITE);
    float body_y = draw_text_wrapped(ren, card_x + 32.0f, card_y + 124.0f, card_w - 64.0f,
                                     "No games currently loaded in library.\nLaunch the First-Time Setup Wizard to configure and import your PSP game.",
                                     1.2f, COLOR_TEXT_MUTED, 4);
    draw_text(ren, card_x + 32.0f, body_y + 4.0f, "Supports registered PSP titles and synthetic test fixtures.", 1.0f, COLOR_TEXT_DIM);

    bool focused = (app->focus_index == 0);
    if (draw_button_focused(ren, card_x + 32.0f, card_y + 240.0f, 260.0f, 48.0f, "START SETUP WIZARD", true, in, focused)) {
        player_app_start_setup_wizard(app);
    }
    draw_text(ren, card_x + 32.0f, card_y + 300.0f, "Enter: Start Wizard  ·  Shortcut: O (Add ISO directly)", 1.0f, COLOR_TEXT_DIM);
}

/* --- View: Loaded Game Library --- */
static void render_loaded_library(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    if (app->game_count <= 0 || app->selected_game_index < 0) {
        render_empty_library(ren, app, in);
        return;
    }
    if (app->selected_game_index >= app->game_count) {
        app->selected_game_index = app->game_count - 1;
    }
    GameRecord *game = &app->games[app->selected_game_index];

    /* Hero Banner Card. Shrinks on short windows so the strip below never
     * falls off screen; wide enough on narrow windows to avoid clipping. */
    float hero_x = 32.0f;
    if (w < 420.0f) hero_x = 16.0f;
    float hero_y = 96.0f;
    float hero_w = w - hero_x * 2.0f;
    if (hero_w < 300.0f) hero_w = w > 32.0f ? w - 32.0f : w;
    float hero_h = 340.0f;
    if (h < 760.0f) hero_h = 300.0f;
    if (h < 680.0f) hero_h = 260.0f;
    /* Very short windows drop the specs rail (its data survives in the
     * settings screen and the strip) so the action buttons and at least
     * one library card stay on screen. */
    bool compact_hero = (h < 620.0f);
    if (compact_hero) hero_h = 215.0f;

    draw_shadow(ren, hero_x, hero_y, hero_w, hero_h, 10.0f);
    draw_rounded_fill(ren, hero_x, hero_y, hero_w, hero_h, 10.0f, COLOR_CARD_BG);
    GameTextureCacheEntry *hero_tex_entry = ui_get_game_texture_entry(game->iso_path);
    if (hero_tex_entry) {
        ui_load_pic1_if_needed(ren, hero_tex_entry, game->iso_path);
        if (hero_tex_entry->pic1_tex) {
            UiClipState saved_clip = ui_clip_save(ren);
            SDL_Rect hero_clip = { (int)hero_x + 1, (int)hero_y + 1, (int)hero_w - 2, (int)hero_h - 2 };
            SDL_SetRenderClipRect(ren, &hero_clip);
            SDL_SetTextureAlphaMod(hero_tex_entry->pic1_tex, 40);
            SDL_FRect dst = { hero_x, hero_y, hero_w, hero_h };
            SDL_RenderTexture(ren, hero_tex_entry->pic1_tex, NULL, &dst);
            draw_filled_rect(ren, hero_x, hero_y, hero_w, hero_h, (SDL_Color){ 12, 15, 18, 120 });
            ui_clip_restore(ren, &saved_clip);
        }
    }
    draw_rounded_outline(ren, hero_x, hero_y, hero_w, hero_h, 10.0f, COLOR_CARD_BORDER);

    /* Status Pills: a successful staging transaction is distinct from runtime
     * preparation. The card must expose that useful intermediate state without
     * claiming that a recompiled child is already available. */
    const char *card_status = game->is_experimental ? "EXPERIMENTAL"
        : ((game->assets_staged && !game->is_prepared)
            ? "ASSETS STAGED" : status_label(game->status));
    draw_badge(ren, hero_x + 32.0f, hero_y + 28.0f, card_status,
               game->is_experimental ? COLOR_AMBER : COLOR_EMERALD);
    if (hero_w >= 560.0f) {
        draw_badge(ren, hero_x + 230.0f, hero_y + 28.0f, game->disc_id, COLOR_BLUE);
    }
    bool showcase_demo = player_game_is_showcase(game);
    if (showcase_demo && hero_w >= 760.0f) {
        draw_badge(ren, hero_x + 350.0f, hero_y + 28.0f, "SHOWCASE DEMO", COLOR_LIME);
    }
    if (hero_w >= 760.0f) {
        draw_badge(ren, hero_x + (showcase_demo ? 510.0f : 350.0f), hero_y + 28.0f,
                   fps_label(app->settings.fps_cap), COLOR_LIME);
    }
    /* Game Icon: loaded from disc ICON0.PNG or generated monogram badge fallback */
    if (hero_w >= 900.0f && hero_h >= 200.0f) {
        draw_game_icon(ren, hero_x + hero_w - 140.0f, hero_y + 24.0f, 108.0f, 64.0f, game);
    }

    /* Game Title */
    draw_text_ellipsized(ren, hero_x + 32.0f, hero_y + 72.0f, game->title_name,
                         2.5f, hero_w - 64.0f, COLOR_TEXT_WHITE);
    if (game->is_experimental) {
        char experimental_reason[224];
        if (game->executable_eboot_kind == NK_ISO_EXEC_PSP_ENCRYPTED &&
            game->executable_selection == NK_ISO_EXEC_SELECTION_NONE) {
            snprintf(experimental_reason, sizeof(experimental_reason),
                     "Executable decryption is in the works (#295). Build the runtime package from the library (#296/#297).");
        } else if (game->selected_executable[0]) {
            snprintf(experimental_reason, sizeof(experimental_reason),
                     "%s selected. Build the runtime package from the library (#296/#297).",
                     game->selected_executable);
        } else {
            snprintf(experimental_reason, sizeof(experimental_reason),
                     "No analyzable executable selected. Build the runtime package from the library (#296/#297).");
        }
        unsigned int issues[5] = { 285, 308, 296, 297, 0 };
        size_t issue_count = 4;
        if (game->executable_eboot_kind == NK_ISO_EXEC_PSP_ENCRYPTED &&
            game->executable_selection == NK_ISO_EXEC_SELECTION_NONE) {
            issues[4] = 295;
            issue_count++;
        }
        if (hero_h <= 215.0f) {
            char compact_summary[400];
            snprintf(compact_summary, sizeof(compact_summary),
                     "Experimental: this game has not been verified. Compatibility is unknown.\n%s",
                     experimental_reason);
            draw_text_wrapped(ren, hero_x + 32.0f, hero_y + 112.0f,
                              hero_w - 64.0f, compact_summary, 0.9f,
                              COLOR_AMBER, 3);
        } else {
            draw_text_wrapped(ren, hero_x + 32.0f, hero_y + 120.0f,
                              hero_w - 64.0f,
                              "Experimental: this game has not been verified. Compatibility is unknown.",
                              1.05f, COLOR_AMBER, 2);
            draw_text_ellipsized(ren, hero_x + 32.0f, hero_y + 156.0f,
                                 experimental_reason, 0.95f, hero_w - 64.0f,
                                 COLOR_TEXT_MUTED);
            draw_issue_links(ren, hero_x + 32.0f,
                             hero_y + (hero_h >= 300.0f ? 184.0f : 174.0f),
                             issues, issue_count, in);
        }
    } else if (hero_h >= 300.0f) {
        draw_text_ellipsized(ren, hero_x + 32.0f, hero_y + 120.0f,
                             "PSP title · Native PC recompilation",
                             1.2f, hero_w - 64.0f, COLOR_TEXT_MUTED);
    }

    /* Quick Specs Rail: three columns on wide heroes, stacked on narrow.
     * Skipped entirely on compact heroes (see above). */
    if (!compact_hero && !game->is_experimental) {
    float rail_y = hero_y + (hero_h >= 300.0f ? 160.0f : 128.0f);
    float rail_h = 60.0f;
    bool stacked_specs = hero_w < 700.0f;
    if (stacked_specs) rail_h = 108.0f;
    if (rail_y + rail_h > hero_y + hero_h - 80.0f) {
        rail_h = hero_y + hero_h - 80.0f - rail_y;
        if (rail_h < 40.0f) rail_h = 40.0f;
    }
    draw_rounded_fill(ren, hero_x + 32.0f, rail_y, hero_w - 64.0f, rail_h, 6.0f, (SDL_Color){ 16, 21, 26, 255 });
    draw_rounded_outline(ren, hero_x + 32.0f, rail_y, hero_w - 64.0f, rail_h, 6.0f, COLOR_CARD_BORDER);

    if (!stacked_specs) {
        float col_w = (hero_w - 96.0f) / 3.0f;
        draw_text(ren, hero_x + 48.0f, rail_y + 12.0f, "RENDER RESOLUTION", 0.9f, COLOR_TEXT_DIM);
        {
            char res_line[64];
            snprintf(res_line, sizeof(res_line), "%s", resolution_label(app->settings.resolution_scale));
            draw_text_ellipsized(ren, hero_x + 48.0f, rail_y + 32.0f, res_line, 1.0f, col_w - 16.0f, COLOR_TEXT_WHITE);
        }
        draw_text(ren, hero_x + 48.0f + col_w, rail_y + 12.0f, "CONTROLLER INPUT", 0.9f, COLOR_TEXT_DIM);
        {
            const char *ctrl_line = app->settings.controller_connected
                ? (app->settings.controller_name[0] ? app->settings.controller_name : "Controller Connected")
                : "Keyboard Ready";
            draw_text_ellipsized(ren, hero_x + 48.0f + col_w, rail_y + 32.0f, ctrl_line, 1.0f, col_w - 16.0f, COLOR_TEXT_WHITE);
        }
        draw_text(ren, hero_x + 48.0f + col_w * 2.0f,
                  rail_y + 12.0f, game->assets_staged ? "STAGED ASSETS" : "SESSION & STORAGE",
                  0.9f, COLOR_TEXT_DIM);
        {
            char save_line[128];
            if (game->assets_staged) {
                snprintf(save_line, sizeof(save_line),
                         "Assets: %u · Audio: %u · Visual: %u",
                         (unsigned)game->extracted_asset_count,
                         (unsigned)game->extracted_audio_count,
                         (unsigned)game->extracted_visual_count);
            } else {
                const char *lp = (game->last_played[0] && strcmp(game->last_played, "Ready") != 0)
                                     ? game->last_played : "Never";
                snprintf(save_line, sizeof(save_line), "Last played: %s", lp);
            }
            draw_text_ellipsized(ren, hero_x + 48.0f + col_w * 2.0f,
                                 rail_y + 32.0f, save_line, 1.0f,
                                 col_w - 16.0f, COLOR_TEXT_WHITE);
        }
    } else {
        char spec_line[128];
        snprintf(spec_line, sizeof(spec_line), "%s · %s", resolution_label(app->settings.resolution_scale),
                 app->settings.controller_connected ? "Gamepad" : "Keyboard");
        draw_text_ellipsized(ren, hero_x + 48.0f, rail_y + 12.0f, spec_line, 1.0f, hero_w - 96.0f, COLOR_TEXT_WHITE);
        {
            char save_line[128];
            if (game->assets_staged) {
                snprintf(save_line, sizeof(save_line),
                         "Assets: %u · Audio: %u · Visual: %u · Layout: %u",
                         (unsigned)game->extracted_asset_count,
                         (unsigned)game->extracted_audio_count,
                         (unsigned)game->extracted_visual_count,
                         (unsigned)game->extracted_layout_count);
            } else {
                const char *lp = (game->last_played[0] && strcmp(game->last_played, "Ready") != 0)
                                     ? game->last_played : "Never";
                snprintf(save_line, sizeof(save_line), "Last played: %s", lp);
            }
            draw_text_ellipsized(ren, hero_x + 48.0f, rail_y + 34.0f,
                                 save_line, 1.0f, hero_w - 96.0f, COLOR_TEXT_MUTED);
        }
        draw_text_ellipsized(ren, hero_x + 48.0f, rail_y + 56.0f, game->iso_path[0] ? game->iso_path : "No source path recorded",
                             0.9f, hero_w - 96.0f, COLOR_TEXT_DIM);
    }
    } /* end specs rail (skipped on compact heroes) */

    /* Action Buttons. Focus order: 0 = primary, 1 = add, 2 = remove,
     * then paging stops. The unavailable pill is never a focus stop. */
    float btn_y = hero_y + hero_h - 62.0f;
    int focus = 0;
    bool primary_focused = (app->focus_index == focus);
    NkRuntimePackageStatus package_status = player_app_validate_runtime_package(
        app, game, NULL, NULL, 0);
    bool package_ready = player_app_game_has_runtime(app, game);
    if (app->is_game_running) {
        char run_str[64];
        snprintf(run_str, sizeof(run_str), "STOP GAME (PID %d)", app->launch_session.process.process_id);
        if (draw_button_focused(ren, hero_x + 32.0f, btn_y, 220.0f, 54.0f, run_str, true, in, primary_focused)) {
            player_app_stop_game(app);
        }
        focus++;
    } else if (package_ready) {
        if (draw_button_focused(ren, hero_x + 32.0f, btn_y, 220.0f, 54.0f,
                                "PLAY NOW", true, in, primary_focused)) {
            player_app_launch_game(app, app->selected_game_index);
        }
        focus++;
    } else if (package_status == NK_RUNTIME_PACKAGE_STALE) {
        if (draw_button_focused(ren, hero_x + 32.0f, btn_y, 220.0f, 54.0f,
                                "REBUILD PACKAGE", true, in, primary_focused)) {
            player_app_start_package_build(app, app->selected_game_index);
        }
        focus++;
    } else if (package_status == NK_RUNTIME_PACKAGE_MISSING) {
        if (draw_button_focused(ren, hero_x + 32.0f, btn_y, 220.0f, 54.0f,
                                "BUILD PACKAGE", true, in, primary_focused)) {
            player_app_start_package_build(app, app->selected_game_index);
        }
        focus++;
    } else if (package_status == NK_RUNTIME_PACKAGE_INCOMPATIBLE) {
        draw_status_pill(ren, hero_x + 32.0f, btn_y, 220.0f, 54.0f,
                         "PACKAGE INCOMPATIBLE (#297)");
    } else if (game->assets_staged) {
        /* Disc extraction is useful progress, but it is not a runnable
         * recompiled title. Keep this state visible without exposing a
         * launch action until the runtime probe succeeds. */
        draw_status_pill(ren, hero_x + 32.0f, btn_y, 220.0f, 54.0f, "RUNTIME REQUIRED");
    } else {
        /* There is no preparation backend in this build. Draw a non-interactive
         * status control so the card does not imply that one is connected. */
        draw_status_pill(ren, hero_x + 32.0f, btn_y, 220.0f, 54.0f, "PREPARATION UNAVAILABLE");
    }

    float add_x = hero_x + 272.0f;
    if (hero_w < 560.0f) add_x = hero_x + 32.0f;
    bool add_focused = (app->focus_index == focus);
    if (draw_button_focused(ren, add_x, btn_y, 220.0f, 54.0f, "ADD ANOTHER ISO", false, in, add_focused)) {
        /* Same dead end as the empty-library button: this control exists to
           pick a file, so it must open the picker. */
        app->request_file_picker = true;
    }
    focus++;

    if (!showcase_demo) {
        bool remove_focused = (app->focus_index == focus);
        if (hero_w >= 760.0f) {
            if (draw_button_focused(ren, hero_x + 512.0f, btn_y, 180.0f, 54.0f, "REMOVE", false, in, remove_focused)) {
                player_app_remove_game(app, app->selected_game_index);
                return;
            }
        } else {
            if (draw_button_focused(ren, hero_x + hero_w - 140.0f, hero_y + 24.0f, 108.0f, 32.0f, "REMOVE", false, in, remove_focused)) {
                player_app_remove_game(app, app->selected_game_index);
                return;
            }
        }
        focus++;
    }

    /* Lower Library Strip.
     *
     * The strip is horizontal and a 1280-wide window fits about four cards.
     * Every entry past that was previously drawn off the right edge of the
     * window, and src/player has no wheel handling, paging, keyboard selection
     * or horizontal offset, so those games could not be selected or launched
     * at all even though the library holds up to NK_MAX_GAMES. The strip now
     * shows a window of cards, always including the selected one, with paging
     * controls and a position readout. */
    float strip_y = hero_y + hero_h + (compact_hero ? 16.0f : 24.0f);
    if (strip_y + 200.0f > h - 32.0f && h > 500.0f) {
        strip_y = h - 32.0f - 200.0f;
    }
    draw_text(ren, 32.0f, strip_y, "INSTALLED TITLES", 1.3f, COLOR_TEXT_WHITE);

    int visible = player_app_visible_library_cards(app);
    int max_scroll = app->game_count - visible;
    if (max_scroll < 0) max_scroll = 0;

    /* Keep the selected card on screen before anything is drawn. */
    if (app->selected_game_index >= 0) {
        if (app->selected_game_index < app->library_scroll_index) {
            app->library_scroll_index = app->selected_game_index;
        } else if (app->selected_game_index >= app->library_scroll_index + visible) {
            app->library_scroll_index = app->selected_game_index - visible + 1;
        }
    }
    if (app->library_scroll_index > max_scroll) app->library_scroll_index = max_scroll;
    if (app->library_scroll_index < 0) app->library_scroll_index = 0;

    int first = app->library_scroll_index;
    int last = first + visible;
    if (last > app->game_count) last = app->game_count;

    for (int i = first; i < last; i++) {
        float card_x = 32.0f + (float)(i - first) * 280.0f;
        float card_y = strip_y + 32.0f;
        float cw = 260.0f;
        float ch = 140.0f;
        if (card_y + ch > h - 70.0f) {
            ch = h - 70.0f - card_y;
            if (ch < 70.0f) break;
        }

        bool active = (i == app->selected_game_index);
        SDL_Color bg = active ? (SDL_Color){ 28, 36, 44, 255 } : COLOR_CARD_BG;
        SDL_Color border = active ? COLOR_EMERALD : COLOR_CARD_BORDER;

        draw_shadow(ren, card_x, card_y, cw, ch, 8.0f);
        draw_rounded_fill(ren, card_x, card_y, cw, ch, 8.0f, bg);
        draw_rounded_outline(ren, card_x, card_y, cw, ch, 8.0f, border);
        if (active && app->focus_index < focus) {
            draw_rounded_outline(ren, card_x - 2.0f, card_y - 2.0f, cw + 4.0f, ch + 4.0f, 10.0f, COLOR_LIME);
        }

        draw_badge(ren, card_x + 12.0f, card_y + 12.0f,
                   player_game_is_showcase(&app->games[i]) ? "SHOWCASE DEMO" : app->games[i].disc_id,
                   active ? COLOR_EMERALD : COLOR_TEXT_DIM);
        float title_avail = cw - 24.0f;
        if (ch >= 80.0f) {
            draw_game_icon(ren, card_x + cw - 64.0f, card_y + 8.0f, 54.0f, 36.0f, &app->games[i]);
            title_avail = cw - 74.0f;
        }
        draw_text_ellipsized(ren, card_x + 12.0f, card_y + 48.0f,
                             app->games[i].title_name, 1.1f, title_avail,
                             COLOR_TEXT_WHITE);
        if (ch >= 110.0f) {
            char status_line[96];
            if (app->games[i].is_prepared) {
                snprintf(status_line, sizeof(status_line), "Status: Prepared");
            } else if (app->games[i].assets_staged) {
                snprintf(status_line, sizeof(status_line), "Assets staged: %u",
                         (unsigned)app->games[i].extracted_asset_count);
            } else {
                snprintf(status_line, sizeof(status_line), "Status: Not prepared");
            }
            draw_text(ren, card_x + 12.0f, card_y + 104.0f,
                      status_line, 0.9f, COLOR_TEXT_MUTED);
        }

        if (in && in->mouse_clicked && is_point_in_rect((float)in->mouse_x, (float)in->mouse_y, card_x, card_y, cw, ch)) {
            app->selected_game_index = i;
        }
    }

    /* Paging controls and a position readout, shown only when the library
     * does not fit. Mouse-only users get buttons; the event loop also maps the
     * arrow keys and the wheel onto the same selection. */
    if (app->game_count > visible) {
        float nav_y = strip_y + 180.0f;
        if (nav_y + 34.0f > h - 32.0f) nav_y = h - 32.0f - 34.0f;
        bool prev_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, 32.0f, nav_y, 60.0f, 34.0f, "<", false, in, prev_focused)) {
            player_app_move_selection(app, -1);
        }
        focus++;
        bool next_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, 100.0f, nav_y, 60.0f, 34.0f, ">", false, in, next_focused)) {
            player_app_move_selection(app, 1);
        }
        focus++;

        char pos_line[64];
        snprintf(pos_line, sizeof(pos_line), "%d of %d  (arrow keys or scroll wheel)",
                 app->selected_game_index >= 0 ? app->selected_game_index + 1 : 0,
                 app->game_count);
        draw_text(ren, 176.0f, nav_y + 8.0f, pos_line, 1.0f, COLOR_TEXT_DIM);
    }
    render_footer_hints(ren, app);
}

/* --- View: Inspecting ISO --- */
static void render_inspecting(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float card_w = dialog_card_w(w, 600.0f);
    float card_h = 300.0f;
    float card_x = centered_card_x(w, card_w);
    if (cx - card_w * 0.5f >= 16.0f) card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;
    if (card_y + card_h > h - 40.0f && h > 460.0f) card_y = h - 40.0f - card_h;

    draw_shadow(ren, card_x, card_y, card_w, card_h, 10.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 32.0f, "ISO INSPECTOR", COLOR_AMBER);
    draw_text(ren, card_x + 32.0f, card_y + 76.0f, "Disc Image Inspection", 2.0f, COLOR_TEXT_WHITE);

    if (app->inspecting_game.iso_path[0] != '\0') {
        /* Real file identity from the inspected entry (populated by --iso=). */
        char size_str[48];
        format_size(size_str, sizeof(size_str), app->inspecting_game.iso_size_bytes);
        float after = draw_text_wrapped(ren, card_x + 32.0f, card_y + 120.0f, card_w - 64.0f,
                                        app->inspecting_game.iso_path, 1.1f, COLOR_TEXT_MUTED, 2);
        char size_line[64];
        snprintf(size_line, sizeof(size_line), "Size: %s", size_str);
        draw_text(ren, card_x + 32.0f, after + 2.0f, size_line, 1.0f, COLOR_TEXT_DIM);
        /* The inspector does not expose progress in this build: an
         * indeterminate indicator that claims no percentage. */
        draw_indeterminate_bar(ren, card_x + 32.0f, card_y + 160.0f, card_w - 64.0f, 16.0f, g_reduce_motion);
        draw_text(ren, card_x + 32.0f, card_y + 190.0f, "Inspecting...", 1.0f, COLOR_TEXT_DIM);
    } else {
        draw_text(ren, card_x + 32.0f, card_y + 120.0f, "No disc image selected.", 1.2f, COLOR_TEXT_MUTED);
        draw_text_wrapped(ren, card_x + 32.0f, card_y + 144.0f, card_w - 64.0f,
                          "Select a lawfully obtained PSP game ISO to inspect it.", 1.0f, COLOR_TEXT_DIM, 2);
    }

    bool focused = (app->focus_index == 0);
    if (draw_button_focused(ren, card_x + 32.0f, card_y + 220.0f, 140.0f, 42.0f, "CANCEL", false, in, focused)) {
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

/* --- View: Supported Title Result --- */
static void render_supported_title(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float card_w = dialog_card_w(w, 680.0f);
    /* Narrow cards stack the actions vertically so BACK can never run
     * off the card edge. Focus order (ADD, BACK) is unchanged. */
    bool stacked_actions = (card_w < 500.0f);
    float card_h = stacked_actions ? 430.0f : 360.0f;
    float card_x = centered_card_x(w, card_w);
    if (cx - card_w * 0.5f >= 16.0f) card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;
    if (card_y + card_h > h - 32.0f && h > 460.0f) card_y = h - 32.0f - card_h;

    draw_shadow(ren, card_x, card_y, card_w, card_h, 10.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 28.0f, "TITLE RECOGNIZED", COLOR_EMERALD);
    if (card_w >= 560.0f) {
        draw_badge(ren, card_x + 220.0f, card_y + 28.0f, app->inspecting_game.disc_id[0] ? app->inspecting_game.disc_id : "DISC_ID", COLOR_BLUE);
    }

    draw_text_ellipsized(ren, card_x + 32.0f, card_y + 72.0f,
                         app->inspecting_game.title_name[0] ? app->inspecting_game.title_name : "PlayStation Portable Title",
                         2.2f, card_w - 64.0f, COLOR_TEXT_WHITE);
    draw_text_wrapped(ren, card_x + 32.0f, card_y + 116.0f, card_w - 64.0f,
                      "Disc identified in Nakagawa title catalog.\nThis build does not connect the module preparation pipeline.",
                      1.1f, COLOR_TEXT_MUTED, 3);

    /* Honesty warning */
    draw_text_wrapped(ren, card_x + 32.0f, card_y + 180.0f, card_w - 64.0f,
                      "NOTE: Full LLE font fidelity requires jpn0.pgf in system font directory.",
                      1.0f, COLOR_AMBER, 2);

    bool add_focused = (app->focus_index == 0);
    if (draw_button_focused(ren, card_x + 32.0f, card_y + 260.0f, 260.0f, 50.0f, "ADD TO LIBRARY", true, in, add_focused)) {
        /* Do not mark the game prepared: no preparation has run. The entry
         * keeps the status reported by the ISO inspection. */
        if (player_app_add_game(app, &app->inspecting_game)) {
            player_app_set_view(app, VIEW_LIBRARY);
        } else {
            /* The add was rejected or never persisted. Saying nothing would
               show the title in the library until the next restart dropped
               it. */
            player_app_set_error(app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                 "The title could not be stored. The library may be full, or the "
                                 "user data directory is not writable.",
                                 "Return to Library", VIEW_LIBRARY);
        }
    }
    bool back_focused = (app->focus_index == 1);
    float back_x = stacked_actions ? card_x + 32.0f : card_x + 310.0f;
    float back_y = stacked_actions ? card_y + 330.0f : card_y + 260.0f;
    if (draw_button_focused(ren, back_x, back_y, 140.0f, 50.0f, "BACK", false, in, back_focused)) {
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

static void render_experimental_title(SDL_Renderer *ren, PlayerApp *app,
                                     const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float card_w = dialog_card_w(w, 760.0f);
    float card_h = h < 620.0f ? 390.0f : 430.0f;
    float card_x = centered_card_x(w, card_w);
    float card_y = (h - card_h) * 0.5f;
    if (card_y < 64.0f) card_y = 64.0f;
    if (card_y + card_h > h - 20.0f && h > 460.0f) card_y = h - 20.0f - card_h;

    draw_shadow(ren, card_x, card_y, card_w, card_h, 10.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_AMBER);
    draw_badge(ren, card_x + 32.0f, card_y + 26.0f, "EXPERIMENTAL", COLOR_AMBER);
    if (card_w >= 560.0f) {
        draw_badge(ren, card_x + 210.0f, card_y + 26.0f,
                   app->inspecting_game.disc_id, COLOR_BLUE);
    }
    draw_text_ellipsized(ren, card_x + 32.0f, card_y + 66.0f,
                         app->inspecting_game.title_name[0]
                             ? app->inspecting_game.title_name
                             : "PlayStation Portable Title",
                         2.0f, card_w - 64.0f, COLOR_TEXT_WHITE);
    float rows_y = draw_text_wrapped(
        ren, card_x + 32.0f, card_y + 106.0f, card_w - 64.0f,
        "Experimental: this game has not been verified. Compatibility is unknown.",
        1.05f, COLOR_AMBER, 2);
    rows_y += 8.0f;

    for (size_t i = 0; i < app->wizard.preflight.count; i++) {
        const PlayerPreflightCheck *check = &app->wizard.preflight.checks[i];
        if (strcmp(check->code, "DISC_SFO") == 0 ||
            strcmp(check->code, "EXPERIMENTAL") == 0) continue;
        const char *status_text = "UNKNOWN";
        SDL_Color status_color = COLOR_TEXT_DIM;
        switch (check->status) {
            case PREFLIGHT_OK: status_text = "OK"; status_color = COLOR_EMERALD; break;
            case PREFLIGHT_MISSING: status_text = "MISSING"; status_color = COLOR_AMBER; break;
            case PREFLIGHT_INCOMPATIBLE: status_text = "INCOMPATIBLE"; status_color = COLOR_RED; break;
            case PREFLIGHT_STALE: status_text = "STALE"; status_color = COLOR_AMBER; break;
            case PREFLIGHT_UNSUPPORTED: status_text = "UNSUPPORTED"; status_color = COLOR_RED; break;
            case PREFLIGHT_IN_PROGRESS: status_text = "IN PROGRESS"; status_color = COLOR_AMBER; break;
            case PREFLIGHT_INVALID: status_text = "INVALID"; status_color = COLOR_RED; break;
        }
        float row_y = rows_y + 27.0f * (float)i;
        draw_text_ellipsized(ren, card_x + 32.0f, row_y,
                             status_text, 0.82f, 82.0f, status_color);
        draw_text_ellipsized(ren, card_x + 120.0f, row_y,
                             check->code, 0.82f, 116.0f, COLOR_TEXT_WHITE);
        float issue_width = (float)check->issue_count * 54.0f;
        float message_width = card_w - 300.0f - issue_width;
        if (message_width < 120.0f) message_width = 120.0f;
        draw_text_ellipsized(ren, card_x + 244.0f, row_y,
                             check->message, 0.78f, message_width,
                             COLOR_TEXT_MUTED);
        if (check->issue_count) {
            draw_issue_links(ren, card_x + card_w - 32.0f - issue_width,
                             row_y - 2.0f, check->issue_numbers,
                             check->issue_count, in);
        }
    }

    float button_y = card_y + card_h - 60.0f;
    bool add_focused = app->focus_index == 0;
    if (draw_button_focused(ren, card_x + 32.0f, button_y, 250.0f, 46.0f,
                            "ADD TO LIBRARY", true, in, add_focused)) {
        if (player_app_add_game(app, &app->inspecting_game)) {
            player_app_set_view(app, VIEW_LIBRARY);
        } else {
            player_app_set_error(app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                 "The experimental title could not be stored. The library may be full, or the user data directory is not writable.",
                                 "Return to Library", VIEW_LIBRARY);
        }
    }
    bool back_focused = app->focus_index == 1;
    if (draw_button_focused(ren, card_x + 300.0f, button_y, 140.0f, 46.0f,
                            "BACK", false, in, back_focused)) {
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

/* --- View: Unsupported Title Result --- */
static void render_unsupported_title(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float card_w = dialog_card_w(w, 640.0f);
    float card_h = 320.0f;
    float card_x = centered_card_x(w, card_w);
    if (cx - card_w * 0.5f >= 16.0f) card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;
    if (card_y + card_h > h - 32.0f && h > 420.0f) card_y = h - 32.0f - card_h;

    draw_shadow(ren, card_x, card_y, card_w, card_h, 10.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_RED);

    draw_badge(ren, card_x + 32.0f, card_y + 32.0f, "UNSUPPORTED TITLE", COLOR_RED);
    draw_text(ren, card_x + 32.0f, card_y + 76.0f, "Title Not Qualified", 2.2f, COLOR_TEXT_WHITE);
    draw_text_wrapped(ren, card_x + 32.0f, card_y + 120.0f, card_w - 64.0f,
                      "The selected ISO disc image is a valid PSP game, but is not yet registered in Nakagawa Recomp's title registry.\nTo avoid unpredictable crashes, unsupported titles are not executed.",
                      1.1f, COLOR_TEXT_MUTED, 4);

    bool focused = (app->focus_index == 0);
    if (draw_button_focused(ren, card_x + 32.0f, card_y + 230.0f, 220.0f, 48.0f, "RETURN TO LIBRARY", true, in, focused)) {
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

/* --- View: Preparation Progress --- */
static void render_preparing(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float card_w = dialog_card_w(w, 700.0f);
    float card_h = 360.0f;
    float card_x = centered_card_x(w, card_w);
    if (cx - card_w * 0.5f >= 16.0f) card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 80.0f) card_y = 80.0f;
    if (card_y + card_h > h - 32.0f && h > 460.0f) card_y = h - 32.0f - card_h;

    draw_shadow(ren, card_x, card_y, card_w, card_h, 10.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 32.0f, "PREPARATION UNAVAILABLE", COLOR_AMBER);
    draw_text(ren, card_x + 32.0f, card_y + 76.0f, "Preparation Unavailable", 2.2f, COLOR_TEXT_WHITE);
    /* This is intentionally an explanation screen, not a fake progress
     * surface. No native preparation pipeline is connected in this build. */
    draw_text_wrapped(ren, card_x + 32.0f, card_y + 120.0f, card_w - 64.0f,
                      "No preparation pipeline is connected in this build.", 1.2f,
                      COLOR_TEXT_MUTED, 2);
    draw_indeterminate_bar(ren, card_x + 32.0f, card_y + 160.0f,
                           card_w - 64.0f, 20.0f, g_reduce_motion);
    draw_text(ren, card_x + 32.0f, card_y + 196.0f,
              "No preparation work is running.", 1.2f, COLOR_TEXT_WHITE);

    bool focused = (app->focus_index == 0);
    if (draw_button_focused(ren, card_x + 32.0f, card_y + 270.0f, 190.0f, 46.0f, "RETURN TO LIBRARY", false, in, focused)) {
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

/* --- View: Settings ---
 *
 * Every control here is live: resolution and frame-cap presets write the
 * launch preferences that PLAY NOW consumes, toggles flip, and volume
 * steps clamp 0..100. Focus order is stable so Tab/Enter and gamepad
 * SOUTH all reach the same actions as a mouse click. */
static void render_settings(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float card_w = w - 64.0f;
    if (card_w < 340.0f) card_w = w > 32.0f ? w - 32.0f : w;
    if (card_w > 1216.0f) card_w = 1216.0f;
    float card_x = centered_card_x(w, card_w);
    float card_y = 96.0f;
    /* Two columns need the four resolution presets (left) to clear the
     * audio column (right); below this the cursor-flow single column
     * below takes over, which cannot overlap by construction. */
    bool two_col = card_w >= 980.0f;
    float card_h = two_col ? 520.0f : 660.0f;
    if (card_y + card_h > h - 40.0f && h > 560.0f) {
        card_h = h - 40.0f - card_y;
        if (card_h < 420.0f) card_h = 420.0f;
    }

    draw_shadow(ren, card_x, card_y, card_w, card_h, 10.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 24.0f, "PLAYER CONFIGURATION", COLOR_BLUE);
    draw_text_ellipsized(ren, card_x + 32.0f, card_y + 60.0f, "Graphics & Controller Settings",
                         2.0f, card_w - 64.0f, COLOR_TEXT_WHITE);
    if (app->settings_notice[0]) {
        draw_text(ren, card_x + 32.0f, card_y + 96.0f, app->settings_notice, 1.0f, COLOR_AMBER);
    } else {
        draw_text(ren, card_x + 32.0f, card_y + 96.0f, "Configuration saved to settings.json.", 1.0f, COLOR_TEXT_DIM);
    }
    /* Identify the platform the player runs, never imply endorsement. */
    draw_text_ellipsized(ren, card_x + 4.0f, card_y + card_h + 14.0f,
                         "Nakagawa Recomp is an independent project, not affiliated with or endorsed by Sony Interactive Entertainment.",
                         0.9f, card_w - 64.0f, COLOR_TEXT_DIM);

    float col1_x = card_x + 32.0f;
    float col2_x = two_col ? card_x + card_w * 0.5f : col1_x;
    float row_y = card_y + 128.0f;
    int focus = 0;

    /* Narrow windows use a cursor-flow single column so sections can never
     * overlap no matter how short the window is. Wide windows keep the
     * two-column arrangement verified in the screenshot matrix. */
    if (!two_col) {
        float inner_r = card_x + card_w - 32.0f;
        float y = row_y;
        /* Resolution: 2x2 grid when four presets do not fit one row. */
        draw_text(ren, col1_x, y, "INTERNAL RENDER RESOLUTION", 1.1f, COLOR_TEXT_DIM);
        {
            struct { const char *label; int scale; } kRes[] = {
                { "1x (480x272)", 1 }, { "2x (Vita)", 2 }, { "4x (1080p)", 4 }, { "8x (4K UHD)", 8 },
            };
            float bx = col1_x;
            float by = y + 24.0f;
            for (int i = 0; i < 4; i++) {
                bool selected = (app->settings.resolution_scale == kRes[i].scale);
                bool focused = (app->focus_index == focus);
                if (bx + 140.0f > inner_r + 1.0f && bx > col1_x) {
                    bx = col1_x;
                    by += 42.0f;
                }
                if (draw_button_focused(ren, bx, by, 140.0f, 36.0f, kRes[i].label, selected, in, focused)) {
                    player_app_set_resolution_scale(app, kRes[i].scale);
                }
                focus++;
                bx += 150.0f;
            }
            y = by + 52.0f;
        }
        /* Frame rate: wrap the same way. */
        draw_text(ren, col1_x, y, "FRAME CADENCE (GAME LAUNCH)", 1.1f, COLOR_TEXT_DIM);
        {
            struct { const char *label; int cap; } kFps[] = {
                { "30 FPS (PSP Cap)", 30 }, { "60 FPS (Smooth)", 60 }, { "Uncapped", 0 },
            };
            float bx = col1_x;
            float by = y + 24.0f;
            for (int i = 0; i < 3; i++) {
                bool selected = (app->settings.fps_cap == kFps[i].cap);
                bool focused = (app->focus_index == focus);
                if (bx + 140.0f > inner_r + 1.0f && bx > col1_x) {
                    bx = col1_x;
                    by += 42.0f;
                }
                if (draw_button_focused(ren, bx, by, 140.0f, 36.0f, kFps[i].label, selected, in, focused)) {
                    player_app_set_fps_cap(app, kFps[i].cap);
                }
                focus++;
                bx += 150.0f;
            }
            y = by + 52.0f;
        }
        /* Display toggles. */
        draw_text(ren, col1_x, y, "DISPLAY (GAME LAUNCH)", 1.1f, COLOR_TEXT_DIM);
        {
            char vsync_label[32];
            snprintf(vsync_label, sizeof(vsync_label), "VSync: %s", app->settings.vsync ? "ON" : "OFF");
            char fs_label[64];
            snprintf(fs_label, sizeof(fs_label), "Fullscreen: %s (applies from a later build)", app->settings.fullscreen ? "ON" : "OFF");
            float by = y + 24.0f;
            float bx = col1_x;
            bool focused = (app->focus_index == focus);
            if (draw_button_focused(ren, bx, by, 140.0f, 36.0f, vsync_label, app->settings.vsync, in, focused)) {
                player_app_toggle_vsync(app);
            }
            focus++;
            bx += 150.0f;
            if (bx + 310.0f > inner_r + 1.0f) {
                bx = col1_x;
                by += 42.0f;
            }
            focused = (app->focus_index == focus);
            if (draw_button_focused(ren, bx, by, 310.0f, 36.0f, fs_label, app->settings.fullscreen, in, focused)) {
                player_app_toggle_fullscreen(app);
            }
            focus++;
            bx += 320.0f;
            if (bx + 200.0f > inner_r + 1.0f) {
                bx = col1_x;
                by += 42.0f;
            }
            char rm_label[40];
            snprintf(rm_label, sizeof(rm_label), "Reduce motion: %s", app->settings.reduce_motion ? "ON" : "OFF");
            focused = (app->focus_index == focus);
            if (draw_button_focused(ren, bx, by, 200.0f, 36.0f, rm_label, app->settings.reduce_motion, in, focused)) {
                player_app_toggle_reduce_motion(app);
            }
            focus++;
            y = by + 52.0f;
        }
        /* Volume stepper and audio text. */
        draw_text(ren, col1_x, y, "AUDIO & SOUND OUTPUT", 1.1f, COLOR_TEXT_DIM);
        y = draw_text_wrapped(ren, col1_x, y + 22.0f, inner_r - col1_x,
                              "Audio output: sound plays when an audio device is present; with none, the game runs silently.",
                              0.95f, COLOR_TEXT_WHITE, 2);
        draw_text(ren, col1_x, y + 6.0f, "MASTER VOLUME (applies from a later build)", 0.9f, COLOR_TEXT_DIM);
        {
            float by = y + 26.0f;
            bool minus_focused = (app->focus_index == focus);
            if (draw_button_focused(ren, col1_x, by, 44.0f, 34.0f, "-", false, in, minus_focused)) {
                player_app_adjust_volume(app, -5);
            }
            focus++;
            float bar_w = inner_r - (col1_x + 54.0f) - 120.0f;
            if (bar_w > 220.0f) bar_w = 220.0f;
            if (bar_w < 80.0f) bar_w = 80.0f;
            draw_progress_bar(ren, col1_x + 54.0f, by + 6.0f, bar_w, 20.0f, (float)app->settings.master_volume);
            bool plus_focused = (app->focus_index == focus);
            if (draw_button_focused(ren, col1_x + 54.0f + bar_w + 10.0f, by, 44.0f, 34.0f, "+", false, in, plus_focused)) {
                player_app_adjust_volume(app, 5);
            }
            focus++;
            char vol_str[32];
            snprintf(vol_str, sizeof(vol_str), "%d%%", app->settings.master_volume);
            draw_text(ren, col1_x + 54.0f + bar_w + 64.0f, by + 4.0f, vol_str, 1.0f, COLOR_TEXT_MUTED);
            y = by + 50.0f;
        }
        /* Gamepad + save stay one-liners here; the topbar badge already
         * carries live controller state and the save path is display-only. */
        if (h >= 620.0f) {
            draw_text(ren, col1_x, y, "GAMEPAD & STORAGE", 1.1f, COLOR_TEXT_DIM);
            if (app->settings.controller_connected) {
                char dev_line[128];
                snprintf(dev_line, sizeof(dev_line), "Device: %s",
                         app->settings.controller_name[0] ? app->settings.controller_name : "Controller");
                draw_text_ellipsized(ren, col1_x, y + 24.0f, dev_line, 1.0f, inner_r - col1_x, COLOR_TEXT_WHITE);
            } else {
                draw_text(ren, col1_x, y + 24.0f, "No controller connected (keyboard ready).", 1.0f, COLOR_TEXT_WHITE);
            }
            draw_text_ellipsized(ren, col1_x, y + 44.0f,
                                 app->settings.save_directory[0] ? app->settings.save_directory : "(not configured)",
                                 1.0f, inner_r - col1_x, COLOR_TEXT_DIM);
            y += 72.0f;
        }
        bool ctrl_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, card_x + 32.0f, y + 8.0f, 220.0f, 36.0f, "CONTROLLER SETTINGS", false, in, ctrl_focused)) {
            player_app_set_view(app, VIEW_CONTROLLER_SETTINGS);
        }
        focus++;

        /* Close in flow: always visible, never overlapping. Grow the card
         * downward to hold it when the window allows. Only the extension
         * is painted: repainting the whole card here would cover the
         * sections drawn above. */
        float close_y = y + 50.0f;
        float want_bottom = close_y + 46.0f + 16.0f;
        if (want_bottom > card_y + card_h && want_bottom <= h - 8.0f) {
            float old_bottom = card_y + card_h;
            draw_filled_rect(ren, card_x, old_bottom - 2.0f, card_w, want_bottom - old_bottom + 2.0f, COLOR_CARD_BG);
            draw_rect_outline(ren, card_x, old_bottom - 1.0f, card_w, want_bottom - old_bottom + 1.0f, COLOR_CARD_BORDER);
            draw_filled_rect(ren, card_x + 1.0f, old_bottom - 1.0f, card_w - 2.0f, 1.0f, COLOR_CARD_BG);
            card_h = want_bottom - card_y;
        }
        bool close_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, card_x + 32.0f, close_y, 200.0f, 46.0f, "SAVE & CLOSE", true, in, close_focused)) {
            player_app_save_settings(app, NULL);
            player_app_set_view(app, VIEW_LIBRARY);
        }
        focus++;
        (void)focus;
        render_footer_hints(ren, app);
        return;
    }
    float col2_y = row_y;

    /* Resolution scale */
    draw_text(ren, col1_x, row_y, "INTERNAL RENDER RESOLUTION", 1.1f, COLOR_TEXT_DIM);
    {
        struct { const char *label; int scale; } kRes[] = {
            { "1x (480x272)", 1 }, { "2x (Vita)", 2 }, { "4x (1080p)", 4 }, { "8x (4K UHD)", 8 },
        };
        float bx = col1_x;
        for (int i = 0; i < 4; i++) {
            bool selected = (app->settings.resolution_scale == kRes[i].scale);
            bool focused = (app->focus_index == focus);
            if (draw_button_focused(ren, bx, row_y + 24.0f, 105.0f, 36.0f, kRes[i].label, selected, in, focused)) {
                player_app_set_resolution_scale(app, kRes[i].scale);
            }
            focus++;
            bx += 113.0f;
        }
    }

    /* Frame rate */
    float fps_y = two_col ? row_y + 90.0f : row_y + 132.0f;
    draw_text(ren, col1_x, fps_y, "FRAME CADENCE (GAME LAUNCH)", 1.1f, COLOR_TEXT_DIM);
    {
        struct { const char *label; int cap; } kFps[] = {
            { "30 FPS (PSP Cap)", 30 }, { "60 FPS (Smooth)", 60 }, { "Uncapped", 0 },
        };
        float bx = col1_x;
        for (int i = 0; i < 3; i++) {
            bool selected = (app->settings.fps_cap == kFps[i].cap);
            bool focused = (app->focus_index == focus);
            if (draw_button_focused(ren, bx, fps_y + 24.0f, 140.0f, 36.0f, kFps[i].label, selected, in, focused)) {
                player_app_set_fps_cap(app, kFps[i].cap);
            }
            focus++;
            bx += 150.0f;
        }
    }

    /* Display toggles */
    float tog_y = fps_y + 90.0f;
    draw_text(ren, col1_x, tog_y, "DISPLAY (GAME LAUNCH)", 1.1f, COLOR_TEXT_DIM);
    {
        char vsync_label[32];
        snprintf(vsync_label, sizeof(vsync_label), "VSync: %s", app->settings.vsync ? "ON" : "OFF");
        bool focused = (app->focus_index == focus);
        if (draw_button_focused(ren, col1_x, tog_y + 24.0f, 130.0f, 36.0f, vsync_label, app->settings.vsync, in, focused)) {
            player_app_toggle_vsync(app);
        }
        focus++;
        char fs_label[64];
        snprintf(fs_label, sizeof(fs_label), "Fullscreen: %s (applies from a later build)", app->settings.fullscreen ? "ON" : "OFF");
        focused = (app->focus_index == focus);
        if (draw_button_focused(ren, col1_x + 140.0f, tog_y + 24.0f, 310.0f, 36.0f, fs_label, app->settings.fullscreen, in, focused)) {
            player_app_toggle_fullscreen(app);
        }
        focus++;
        char rm_label[40];
        snprintf(rm_label, sizeof(rm_label), "Reduce motion: %s", app->settings.reduce_motion ? "ON" : "OFF");
        focused = (app->focus_index == focus);
        if (draw_button_focused(ren, col1_x, tog_y + 68.0f, 210.0f, 36.0f, rm_label, app->settings.reduce_motion, in, focused)) {
            player_app_toggle_reduce_motion(app);
        }
        focus++;
    }

    /* Audio */
    draw_text(ren, col2_x, col2_y, "AUDIO & SOUND OUTPUT", 1.1f, COLOR_TEXT_DIM);
    draw_text_wrapped(ren, col2_x, col2_y + 24.0f, card_x + card_w - 32.0f - col2_x,
                      "Audio output: sound plays when an audio device is present; with none, the game runs silently.",
                      1.0f, COLOR_TEXT_WHITE, 2);
    draw_text(ren, col2_x, col2_y + 70.0f, "MASTER VOLUME (applies from a later build)", 0.9f, COLOR_TEXT_DIM);
    {
        bool minus_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, col2_x, col2_y + 86.0f, 44.0f, 34.0f, "-", false, in, minus_focused)) {
            player_app_adjust_volume(app, -5);
        }
        focus++;
        float bar_w = 200.0f;
        if (card_x + card_w - 32.0f - (col2_x + 54.0f) - 120.0f < bar_w) {
            bar_w = card_x + card_w - 32.0f - (col2_x + 54.0f) - 120.0f;
            if (bar_w < 100.0f) bar_w = 100.0f;
        }
        draw_progress_bar(ren, col2_x + 54.0f, col2_y + 92.0f, bar_w, 20.0f, (float)app->settings.master_volume);
        bool plus_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, col2_x + 54.0f + bar_w + 10.0f, col2_y + 86.0f, 44.0f, 34.0f, "+", false, in, plus_focused)) {
            player_app_adjust_volume(app, 5);
        }
        focus++;
        char vol_str[32];
        snprintf(vol_str, sizeof(vol_str), "%d%%", app->settings.master_volume);
        draw_text(ren, col2_x + 54.0f + bar_w + 64.0f, col2_y + 90.0f, vol_str, 1.0f, COLOR_TEXT_MUTED);
    }

    /* Controller */
    float pad_y = col2_y + 140.0f;
    draw_text(ren, col2_x, pad_y, "GAMEPAD", 1.1f, COLOR_TEXT_DIM);
    if (app->settings.controller_connected) {
        char dev_line[128];
        snprintf(dev_line, sizeof(dev_line), "Device: %s", app->settings.controller_name[0] ? app->settings.controller_name : "Controller");
        draw_text_ellipsized(ren, col2_x, pad_y + 24.0f, dev_line, 1.1f,
                             card_x + card_w - 32.0f - col2_x, COLOR_TEXT_WHITE);
    } else {
        draw_text_wrapped(ren, col2_x, pad_y + 24.0f, card_x + card_w - 32.0f - col2_x,
                          "No controller connected (keyboard ready). Connect a pad; d-pad moves, A activates, B goes back.",
                          1.0f, COLOR_TEXT_WHITE, 2);
    }
    /* Controller Settings button (#357) */
    bool ctrl_focused = (app->focus_index == focus);
    if (draw_button_focused(ren, col2_x, pad_y + 48.0f, 220.0f, 34.0f, "CONTROLLER SETTINGS", false, in, ctrl_focused)) {
        player_app_set_view(app, VIEW_CONTROLLER_SETTINGS);
    }
    focus++;

    /* Save path lives under the gamepad block: column one grew a second
     * toggle row, so its old slot now belongs to reduce-motion. */
    draw_text(ren, col2_x, pad_y + 92.0f, "STORAGE & SAVE DIRECTORY", 1.1f, COLOR_TEXT_DIM);
    draw_text_ellipsized(ren, col2_x, pad_y + 116.0f,
                         app->settings.save_directory[0] ? app->settings.save_directory : "(not configured)",
                         1.1f, card_x + card_w - 32.0f - col2_x, COLOR_TEXT_WHITE);

    /* Close */
    bool close_focused = (app->focus_index == focus);
    float close_y = card_y + card_h - 72.0f;
    if (draw_button_focused(ren, card_x + 32.0f, close_y, 200.0f, 46.0f, "SAVE & CLOSE", true, in, close_focused)) {
        player_app_save_settings(app, NULL);
        player_app_set_view(app, VIEW_LIBRARY);
    }
    focus++;
    (void)focus;
    render_footer_hints(ren, app);
}

/* --- View: Controller Settings (#357) --- */
static void render_controller_settings(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float card_w = w - 64.0f;
    if (card_w < 340.0f) card_w = w > 32.0f ? w - 32.0f : w;
    if (card_w > 1216.0f) card_w = 1216.0f;
    float card_x = centered_card_x(w, card_w);
    float card_y = 60.0f;
    bool two_col = card_w >= 880.0f;
    float card_h = two_col ? 540.0f : 860.0f;
    if (card_y + card_h > h - 40.0f && h > 560.0f) {
        card_h = h - 40.0f - card_y;
        if (card_h < 440.0f) card_h = 440.0f;
    }

    draw_shadow(ren, card_x, card_y, card_w, card_h, 10.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BORDER);

    draw_badge(ren, card_x + 32.0f, card_y + 20.0f, "CONTROLLER CONFIGURATION", COLOR_BLUE);
    draw_text_ellipsized(ren, card_x + 32.0f, card_y + 48.0f, "PSP Controller Mapping & Calibration",
                         1.8f, card_w - 64.0f, COLOR_TEXT_WHITE);

    if (app->settings.controller_connected) {
        char dev_line[128];
        snprintf(dev_line, sizeof(dev_line), "Connected: %s",
                 app->settings.controller_name[0] ? app->settings.controller_name : "Controller");
        draw_text_ellipsized(ren, card_x + 32.0f, card_y + 76.0f, dev_line, 0.95f, card_w - 64.0f, COLOR_LIME);
    } else {
        draw_text(ren, card_x + 32.0f, card_y + 76.0f,
                  "No controller connected (keyboard navigation active; defaults shown)",
                  0.95f, COLOR_TEXT_DIM);
    }

    float content_y = card_y + 98.0f;
    if (input_settings_has_conflicts(&app->input_settings)) {
        const char *conf = input_settings_get_conflict_summary(&app->input_settings);
        draw_rounded_outline(ren, card_x + 32.0f, content_y, card_w - 64.0f, 24.0f, 4.0f, COLOR_RED);
        draw_text_ellipsized(ren, card_x + 40.0f, content_y + 5.0f, conf, 0.85f, card_w - 80.0f, COLOR_RED);
        content_y += 30.0f;
    } else if (app->input_settings.has_load_diagnostic) {
        char diag_line[384];
        snprintf(diag_line, sizeof(diag_line), "Load Diagnostic: %s", app->input_settings.load_diagnostic);
        draw_text_ellipsized(ren, card_x + 32.0f, content_y + 4.0f, diag_line, 0.85f, card_w - 64.0f, COLOR_AMBER);
        content_y += 24.0f;
    } else if (app->input_settings.has_save_diagnostic) {
        char diag_line[384];
        snprintf(diag_line, sizeof(diag_line), "Status: %s", app->input_settings.save_diagnostic);
        draw_text_ellipsized(ren, card_x + 32.0f, content_y + 4.0f, diag_line, 0.85f, card_w - 64.0f, COLOR_TEXT_MUTED);
        content_y += 24.0f;
    }

    if (input_settings_is_calibrating(&app->input_settings)) {
        GuidedCalibrationStage stage = input_settings_get_calibration_stage(&app->input_settings);
        float cal_card_x = card_x + 32.0f;
        float cal_card_y = content_y + 8.0f;
        float cal_card_w = card_w - 64.0f;
        float cal_btn_y = card_y + card_h - 52.0f;

        switch (stage) {
            case CALIBRATION_STAGE_REST: {
                draw_badge(ren, cal_card_x, cal_card_y, "STEP 1 OF 3: RESTING POSITIONS", COLOR_BLUE);
                draw_text(ren, cal_card_x, cal_card_y + 30.0f, "Leave Analog Stick and Triggers at Rest", 1.8f, COLOR_TEXT_WHITE);
                draw_text_wrapped(ren, cal_card_x, cal_card_y + 70.0f, cal_card_w,
                                  "Please do not touch the analog stick or triggers for about 1 second.\n"
                                  "The system is sampling the neutral resting values of your controller hardware.",
                                  1.1f, COLOR_TEXT_MUTED, 3);
                int ms = app->input_settings.calib.elapsed_ms;
                if (ms > 1000) ms = 1000;
                float pct = (float)ms / 10.0f;
                float bar_w = cal_card_w > 400.0f ? 400.0f : cal_card_w;
                draw_progress_bar(ren, cal_card_x, cal_card_y + 140.0f, bar_w, 24.0f, pct);
                char prog_text[64];
                snprintf(prog_text, sizeof(prog_text), "Sampling resting values: %d / 1000 ms (%d%%)", ms, (int)pct);
                draw_text(ren, cal_card_x, cal_card_y + 175.0f, prog_text, 1.0f, COLOR_TEXT_DIM);

                char live_rest_info[256];
                snprintf(live_rest_info, sizeof(live_rest_info),
                         "Live Neutral: Stick X=%d, Stick Y=%d | L-Trig=%d, R-Trig=%d",
                         app->input_settings.calib.sampled_rest_x,
                         app->input_settings.calib.sampled_rest_y,
                         app->input_settings.calib.sampled_rest_lt,
                         app->input_settings.calib.sampled_rest_rt);
                draw_text(ren, cal_card_x, cal_card_y + 205.0f, live_rest_info, 0.95f, COLOR_LIME);

                bool cancel_focused = (app->focus_index == 0);
                if (draw_button_focused(ren, cal_card_x, cal_btn_y, 220.0f, 38.0f, "CANCEL CALIBRATION", false, in, cancel_focused)) {
                    input_settings_cancel_calibration(&app->input_settings);
                }
                break;
            }
            case CALIBRATION_STAGE_EXTREMES: {
                draw_badge(ren, cal_card_x, cal_card_y, "STEP 2 OF 3: SAMPLE EXTREMES", COLOR_AMBER);
                draw_text(ren, cal_card_x, cal_card_y + 30.0f, "Move Stick in Circles & Press Triggers Fully", 1.8f, COLOR_TEXT_WHITE);
                draw_text_wrapped(ren, cal_card_x, cal_card_y + 70.0f, cal_card_w,
                                  "Rotate the analog stick in full circles around its boundaries,\n"
                                  "and squeeze both the left and right triggers down completely.\n"
                                  "When finished capturing maximum ranges, click 'Finish Sampling'.",
                                  1.1f, COLOR_TEXT_MUTED, 4);

                char ext_stick_info[256];
                snprintf(ext_stick_info, sizeof(ext_stick_info),
                         "Stick X: min = %6d, max = %6d (live: %6d)\nStick Y: min = %6d, max = %6d (live: %6d)",
                         app->input_settings.calib.sampled_min_x,
                         app->input_settings.calib.sampled_max_x,
                         app->host_axes_live[NK_HOST_AXIS_LEFTX],
                         app->input_settings.calib.sampled_min_y,
                         app->input_settings.calib.sampled_max_y,
                         app->host_axes_live[NK_HOST_AXIS_LEFTY]);
                draw_text_wrapped(ren, cal_card_x, cal_card_y + 155.0f, cal_card_w, ext_stick_info, 1.0f, COLOR_TEXT_WHITE, 2);

                char ext_trig_info[256];
                snprintf(ext_trig_info, sizeof(ext_trig_info),
                         "Left Trigger:  rest = %6d, extreme = %6d (live: %6d)\nRight Trigger: rest = %6d, extreme = %6d (live: %6d)",
                         app->input_settings.calib.sampled_rest_lt,
                         app->input_settings.calib.sampled_max_lt,
                         app->host_axes_live[NK_HOST_AXIS_LEFT_TRIGGER],
                         app->input_settings.calib.sampled_rest_rt,
                         app->input_settings.calib.sampled_max_rt,
                         app->host_axes_live[NK_HOST_AXIS_RIGHT_TRIGGER]);
                draw_text_wrapped(ren, cal_card_x, cal_card_y + 205.0f, cal_card_w, ext_trig_info, 1.0f, COLOR_TEXT_WHITE, 2);

                bool finish_focused = (app->focus_index == 0);
                if (draw_button_focused(ren, cal_card_x, cal_btn_y, 200.0f, 38.0f, "FINISH SAMPLING", true, in, finish_focused)) {
                    input_settings_finish_calibration_extremes(&app->input_settings);
                }

                bool cancel_focused = (app->focus_index == 1);
                if (draw_button_focused(ren, cal_card_x + 220.0f, cal_btn_y, 140.0f, 38.0f, "CANCEL", false, in, cancel_focused)) {
                    input_settings_cancel_calibration(&app->input_settings);
                }
                break;
            }
            case CALIBRATION_STAGE_RESULT: {
                draw_badge(ren, cal_card_x, cal_card_y, "STEP 3 OF 3: REVIEW & ACCEPT", COLOR_EMERALD);
                draw_text(ren, cal_card_x, cal_card_y + 30.0f, "Review Calibration Results", 1.8f, COLOR_TEXT_WHITE);
                draw_text_wrapped(ren, cal_card_x, cal_card_y + 70.0f, cal_card_w,
                                  "Check the recorded neutral resting points and extreme ranges below.\n"
                                  "Click 'Accept Calibration' to apply these transforms to the active profile.",
                                  1.1f, COLOR_TEXT_MUTED, 3);

                char res_x[128], res_y[128], res_tl[128], res_tr[128];
                snprintf(res_x, sizeof(res_x), "Stick X-Axis:  Rest = %6d,  Min = %6d,  Max = %6d",
                         app->input_settings.calib.result_rest_x,
                         app->input_settings.calib.result_min_x,
                         app->input_settings.calib.result_max_x);
                snprintf(res_y, sizeof(res_y), "Stick Y-Axis:  Rest = %6d,  Min = %6d,  Max = %6d",
                         app->input_settings.calib.result_rest_y,
                         app->input_settings.calib.result_min_y,
                         app->input_settings.calib.result_max_y);
                snprintf(res_tl, sizeof(res_tl), "Left Trigger:  Rest = %6d,  Extreme = %6d",
                         app->input_settings.calib.result_trigger_rest,
                         app->input_settings.calib.result_trigger_extreme);
                snprintf(res_tr, sizeof(res_tr), "Right Trigger: Rest = %6d,  Extreme = %6d",
                         app->input_settings.calib.result_trigger_rest,
                         app->input_settings.calib.result_trigger_extreme);

                draw_text(ren, cal_card_x, cal_card_y + 140.0f, res_x, 1.05f, COLOR_TEXT_WHITE);
                draw_text(ren, cal_card_x, cal_card_y + 170.0f, res_y, 1.05f, COLOR_TEXT_WHITE);
                draw_text(ren, cal_card_x, cal_card_y + 200.0f, res_tl, 1.05f, COLOR_LIME);
                draw_text(ren, cal_card_x, cal_card_y + 230.0f, res_tr, 1.05f, COLOR_LIME);

                bool accept_focused = (app->focus_index == 0);
                if (draw_button_focused(ren, cal_card_x, cal_btn_y, 220.0f, 38.0f, "ACCEPT CALIBRATION", true, in, accept_focused)) {
                    input_settings_accept_calibration(&app->input_settings);
                }

                bool cancel_focused = (app->focus_index == 1);
                if (draw_button_focused(ren, cal_card_x + 240.0f, cal_btn_y, 140.0f, 38.0f, "CANCEL", false, in, cancel_focused)) {
                    input_settings_cancel_calibration(&app->input_settings);
                }
                break;
            }
            default:
                break;
        }

        render_footer_hints(ren, app);
        return;
    }

    int focus = 0;

    if (!two_col) {
        /* Single column layout for narrow windows */
        float inner_r = card_x + card_w - 32.0f;
        float y = content_y;

        draw_text(ren, card_x + 32.0f, y, "PSP DIGITAL BUTTONS (CLICK TO REBIND)", 1.0f, COLOR_TEXT_DIM);
        y += 22.0f;

        for (int i = 0; i < INPUT_CONTROL_DIGITAL_COUNT; i++) {
            char btn_label[128];
            bool capturing = (app->input_settings.capturing && app->input_settings.capture_control == i);
            if (capturing) {
                int rem_sec = (input_settings_get_capture_remaining_ms(&app->input_settings) + 999) / 1000;
                snprintf(btn_label, sizeof(btn_label), "%s: PRESS BUTTON... (%ds)",
                         input_settings_control_name(i), rem_sec);
            } else {
                char bind_str[64] = {0};
                input_settings_format_binding(&app->input_settings, i, bind_str, sizeof(bind_str));
                snprintf(btn_label, sizeof(btn_label), "%s: %s", input_settings_control_name(i), bind_str);
            }

            bool focused = (app->focus_index == focus);
            if (draw_button_focused(ren, card_x + 32.0f, y, inner_r - card_x - 32.0f, 26.0f,
                                    btn_label, capturing, in, focused)) {
                if (capturing) {
                    input_settings_cancel_capture(&app->input_settings);
                } else {
                    input_settings_start_capture(&app->input_settings, i);
                }
            }
            focus++;
            y += 29.0f;
        }

        /* Deadzone stepper */
        int dz = app->input_settings.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner;
        char dz_str[64];
        snprintf(dz_str, sizeof(dz_str), "Stick Deadzone: %d (%d%%)", dz, (dz * 100) / 32767);
        draw_text(ren, card_x + 32.0f, y, dz_str, 0.95f, COLOR_TEXT_DIM);
        y += 18.0f;

        bool minus_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, card_x + 32.0f, y, 40.0f, 28.0f, "-", false, in, minus_focused)) {
            input_settings_adjust_deadzone(&app->input_settings, -1, -1000);
        }
        focus++;

        bool plus_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, card_x + 80.0f, y, 40.0f, 28.0f, "+", false, in, plus_focused)) {
            input_settings_adjust_deadzone(&app->input_settings, -1, 1000);
        }
        focus++;
        y += 36.0f;

        /* Trigger threshold stepper */
        int th = app->input_settings.profile.trigger_threshold;
        char th_str[64];
        snprintf(th_str, sizeof(th_str), "Trigger Threshold: %d (%d%%)", th, (th * 100) / 32767);
        draw_text(ren, card_x + 32.0f, y, th_str, 0.95f, COLOR_TEXT_DIM);
        y += 18.0f;

        bool trig_minus_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, card_x + 32.0f, y, 40.0f, 28.0f, "-", false, in, trig_minus_focused)) {
            input_settings_adjust_trigger_threshold(&app->input_settings, -1000);
        }
        focus++;

        bool trig_plus_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, card_x + 80.0f, y, 40.0f, 28.0f, "+", false, in, trig_plus_focused)) {
            input_settings_adjust_trigger_threshold(&app->input_settings, 1000);
        }
        focus++;
        y += 36.0f;

        bool cal_btn_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, card_x + 32.0f, y, 260.0f, 32.0f, "CALIBRATE STICK & TRIGGERS", false, in, cal_btn_focused)) {
            input_settings_start_calibration(&app->input_settings);
        }
        focus++;
        y += 40.0f;

        /* Bottom actions */
        bool save_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, card_x + 32.0f, y, 150.0f, 36.0f, "SAVE PROFILE", true, in, save_focused)) {
            NkResult sres = input_settings_save(&app->input_settings, NULL);
            if (sres == NK_OK) {
                snprintf(app->input_settings.save_diagnostic, sizeof(app->input_settings.save_diagnostic),
                         "Profile successfully saved to disk");
                app->input_settings.has_save_diagnostic = true;
            }
        }
        focus++;

        bool reset_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, card_x + 190.0f, y, 150.0f, 36.0f, "RESET DEFAULTS", false, in, reset_focused)) {
            input_settings_reset_to_defaults(&app->input_settings);
        }
        focus++;

        bool back_focused = (app->focus_index == focus);
        if (draw_button_focused(ren, card_x + 350.0f, y, 160.0f, 36.0f, "BACK TO SETTINGS", false, in, back_focused)) {
            if (input_settings_is_capturing(&app->input_settings)) {
                input_settings_cancel_capture(&app->input_settings);
            }
            player_app_set_view(app, VIEW_SETTINGS);
        }
        focus++;

        (void)focus;
        render_footer_hints(ren, app);
        return;
    }

    /* Two-column layout */
    float left_w = (card_w - 88.0f) * 0.58f;
    float right_x = card_x + 32.0f + left_w + 24.0f;
    float right_w = card_x + card_w - 32.0f - right_x;

    /* Left column: 14 digital controls arranged in 2 sub-columns of 7 rows */
    draw_text(ren, card_x + 32.0f, content_y, "PSP DIGITAL BUTTONS (CLICK TO REBIND)", 1.0f, COLOR_TEXT_DIM);
    float list_y = content_y + 24.0f;
    float subcol_w = (left_w - 12.0f) * 0.5f;

    for (int i = 0; i < INPUT_CONTROL_DIGITAL_COUNT; i++) {
        int col = (i < 7) ? 0 : 1;
        int row = (i < 7) ? i : (i - 7);
        float bx = (col == 0) ? (card_x + 32.0f) : (card_x + 32.0f + subcol_w + 12.0f);
        float by = list_y + (float)row * 36.0f;

        char btn_label[96];
        bool capturing = (app->input_settings.capturing && app->input_settings.capture_control == i);
        if (capturing) {
            int rem_sec = (input_settings_get_capture_remaining_ms(&app->input_settings) + 999) / 1000;
            snprintf(btn_label, sizeof(btn_label), "%s: PRESS... (%ds)",
                     input_settings_control_name(i), rem_sec);
        } else {
            char bind_str[64] = {0};
            input_settings_format_binding(&app->input_settings, i, bind_str, sizeof(bind_str));
            snprintf(btn_label, sizeof(btn_label), "%s: %s", input_settings_control_name(i), bind_str);
        }

        bool focused = (app->focus_index == focus);
        if (input_settings_is_control_conflicted(&app->input_settings, i)) {
            draw_rounded_outline(ren, bx - 1.0f, by - 1.0f, subcol_w + 2.0f, 32.0f, 5.0f, COLOR_RED);
        }

        if (draw_button_focused(ren, bx, by, subcol_w, 30.0f, btn_label, capturing, in, focused)) {
            if (capturing) {
                input_settings_cancel_capture(&app->input_settings);
            } else {
                input_settings_start_capture(&app->input_settings, i);
            }
        }
        focus++;
    }

    /* Right column: Deadzone & Sensitivity */
    float cal_y = content_y;
    draw_text(ren, right_x, cal_y, "STICK & TRIGGER CALIBRATION", 1.0f, COLOR_TEXT_DIM);

    /* Deadzone Stepper */
    int dz = app->input_settings.profile.axes[NK_PSP_AXIS_ANALOG_X].deadzone_inner;
    char dz_str[64];
    snprintf(dz_str, sizeof(dz_str), "Stick Deadzone: %d (%d%%)", dz, (dz * 100) / 32767);
    draw_text(ren, right_x, cal_y + 24.0f, dz_str, 0.95f, COLOR_TEXT_WHITE);

    bool minus_focused = (app->focus_index == focus);
    if (draw_button_focused(ren, right_x, cal_y + 44.0f, 44.0f, 30.0f, "-", false, in, minus_focused)) {
        input_settings_adjust_deadzone(&app->input_settings, -1, -1000);
    }
    focus++;

    bool plus_focused = (app->focus_index == focus);
    if (draw_button_focused(ren, right_x + 52.0f, cal_y + 44.0f, 44.0f, 30.0f, "+", false, in, plus_focused)) {
        input_settings_adjust_deadzone(&app->input_settings, -1, 1000);
    }
    focus++;

    /* Trigger Threshold Stepper */
    int th = app->input_settings.profile.trigger_threshold;
    char th_str[64];
    snprintf(th_str, sizeof(th_str), "Trigger Threshold: %d (%d%%)", th, (th * 100) / 32767);
    draw_text(ren, right_x + 220.0f, cal_y + 24.0f, th_str, 0.95f, COLOR_TEXT_WHITE);

    bool trig_minus_focused = (app->focus_index == focus);
    if (draw_button_focused(ren, right_x + 220.0f, cal_y + 44.0f, 44.0f, 30.0f, "-", false, in, trig_minus_focused)) {
        input_settings_adjust_trigger_threshold(&app->input_settings, -1000);
    }
    focus++;

    bool trig_plus_focused = (app->focus_index == focus);
    if (draw_button_focused(ren, right_x + 272.0f, cal_y + 44.0f, 44.0f, 30.0f, "+", false, in, trig_plus_focused)) {
        input_settings_adjust_trigger_threshold(&app->input_settings, 1000);
    }
    focus++;

    bool cal_btn_focused = (app->focus_index == focus);
    if (draw_button_focused(ren, right_x, cal_y + 82.0f, 260.0f, 32.0f, "CALIBRATE STICK & TRIGGERS", false, in, cal_btn_focused)) {
        input_settings_start_calibration(&app->input_settings);
    }
    focus++;

    /* Live Monitor & Deadzone Visualizer Box */
    float mon_y = cal_y + 122.0f;
    draw_text(ren, right_x, mon_y, "LIVE INPUT & DEADZONE MONITOR", 1.0f, COLOR_TEXT_DIM);

    int16_t rx = app->host_axes_live[NK_HOST_AXIS_LEFTX];
    int16_t ry = app->host_axes_live[NK_HOST_AXIS_LEFTY];
    uint8_t psp_x = 128, psp_y = 128;
    nk_input_profile_eval_analog(&app->input_settings.profile, app->host_axes_live, &psp_x, &psp_y);

    char coord_buf[128];
    snprintf(coord_buf, sizeof(coord_buf), "Raw: (%6d, %6d)  PSP: (%3u, %3u)", rx, ry, psp_x, psp_y);
    bool stick_active = (psp_x != 128 || psp_y != 128);
    draw_text(ren, right_x, mon_y + 22.0f, coord_buf, 0.88f, stick_active ? COLOR_LIME : COLOR_TEXT_MUTED);

    /* Deadzone Box */
    float box_sz = 100.0f;
    float box_x = right_x;
    float box_y = mon_y + 42.0f;
    float center_x = box_x + box_sz * 0.5f;
    float center_y = box_y + box_sz * 0.5f;

    draw_rounded_fill(ren, box_x, box_y, box_sz, box_sz, 6.0f, (SDL_Color){ 16, 22, 28, 255 });
    draw_rounded_outline(ren, box_x, box_y, box_sz, box_sz, 6.0f, COLOR_CARD_BORDER);

    /* Crosshairs */
    set_draw_color(ren, (SDL_Color){ 50, 60, 72, 255 });
    SDL_RenderLine(ren, box_x + 6.0f, center_y, box_x + box_sz - 6.0f, center_y);
    SDL_RenderLine(ren, center_x, box_y + 6.0f, center_x, box_y + box_sz - 6.0f);

    /* Inner deadzone boundary box */
    float dz_half = (float)dz / 32767.0f * 44.0f;
    if (dz_half < 2.0f) dz_half = 2.0f;
    set_draw_color(ren, (SDL_Color){ 70, 130, 180, 200 });
    SDL_FRect dz_rect = { center_x - dz_half, center_y - dz_half, dz_half * 2.0f, dz_half * 2.0f };
    SDL_RenderRect(ren, &dz_rect);

    /* Live stick position dot */
    float dot_x = center_x + (float)rx / 32767.0f * 44.0f;
    float dot_y = center_y + (float)ry / 32767.0f * 44.0f;
    SDL_Color dot_col = stick_active ? COLOR_LIME : COLOR_TEXT_WHITE;
    draw_filled_rect(ren, dot_x - 3.0f, dot_y - 3.0f, 6.0f, 6.0f, dot_col);

    /* Status badge beside visualizer */
    float stat_x = box_x + box_sz + 16.0f;
    if (stick_active) {
        draw_badge(ren, stat_x, box_y + 10.0f, "STICK: ACTIVE", COLOR_LIME);
    } else {
        draw_badge(ren, stat_x, box_y + 10.0f, "STICK: DEADZONE", COLOR_TEXT_DIM);
    }

    char trig_live[64];
    snprintf(trig_live, sizeof(trig_live), "Triggers: L=%d R=%d",
             app->host_axes_live[NK_HOST_AXIS_LEFT_TRIGGER],
             app->host_axes_live[NK_HOST_AXIS_RIGHT_TRIGGER]);
    draw_text(ren, stat_x, box_y + 40.0f, trig_live, 0.85f, COLOR_TEXT_MUTED);

    /* Pressed buttons summary */
    char down_buf[128] = "Pressed: ";
    int down_count = 0;
    for (int b = 0; b < NK_HOST_BUTTON_COUNT && down_count < 4; b++) {
        if (app->host_buttons_live[b]) {
            if (down_count > 0) strncat(down_buf, ", ", sizeof(down_buf) - strlen(down_buf) - 1);
            strncat(down_buf, nk_host_button_name((NkHostGamepadButton)b), sizeof(down_buf) - strlen(down_buf) - 1);
            down_count++;
        }
    }
    if (down_count == 0) strncat(down_buf, "(none)", sizeof(down_buf) - strlen(down_buf) - 1);
    draw_text_ellipsized(ren, stat_x, box_y + 64.0f, down_buf, 0.85f, right_w - box_sz - 24.0f, COLOR_TEXT_MUTED);

    /* Bottom Action Bar */
    float bottom_y = card_y + card_h - 52.0f;

    bool save_focused = (app->focus_index == focus);
    if (draw_button_focused(ren, card_x + 32.0f, bottom_y, 160.0f, 38.0f, "SAVE PROFILE", true, in, save_focused)) {
        NkResult sres = input_settings_save(&app->input_settings, NULL);
        if (sres == NK_OK) {
            snprintf(app->input_settings.save_diagnostic, sizeof(app->input_settings.save_diagnostic),
                     "Profile successfully saved to disk");
            app->input_settings.has_save_diagnostic = true;
        }
    }
    focus++;

    bool reset_focused = (app->focus_index == focus);
    if (draw_button_focused(ren, card_x + 208.0f, bottom_y, 160.0f, 38.0f, "RESET DEFAULTS", false, in, reset_focused)) {
        input_settings_reset_to_defaults(&app->input_settings);
    }
    focus++;

    bool back_focused = (app->focus_index == focus);
    if (draw_button_focused(ren, card_x + 384.0f, bottom_y, 180.0f, 38.0f, "BACK TO SETTINGS", false, in, back_focused)) {
        if (input_settings_is_capturing(&app->input_settings)) {
            input_settings_cancel_capture(&app->input_settings);
        }
        player_app_set_view(app, VIEW_SETTINGS);
    }
    focus++;

    (void)focus;
    render_footer_hints(ren, app);
}

/* --- View: Building Runtime Package --- */
static void render_building_package(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float card_w = dialog_card_w(w, 720.0f);
    float card_h = 460.0f;
    if (card_h > h - 40.0f && h > 300.0f) card_h = h - 40.0f;
    float card_x = centered_card_x(w, card_w);
    if (cx - card_w * 0.5f >= 16.0f) card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 70.0f) card_y = 70.0f;

    draw_shadow(ren, card_x, card_y, card_w, card_h, 10.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BORDER);

    /* Badge & Title */
    draw_badge(ren, card_x + 32.0f, card_y + 28.0f, "BUILDING RUNTIME PACKAGE", COLOR_BLUE);

    char title_buf[256];
    if (app->build_session.title_name[0]) {
        snprintf(title_buf, sizeof(title_buf), "%s (%s)",
                 app->build_session.title_name, app->build_session.disc_id);
    } else {
        snprintf(title_buf, sizeof(title_buf), "Package Build (%s)",
                 app->build_session.disc_id);
    }
    draw_text_ellipsized(ren, card_x + 32.0f, card_y + 64.0f, title_buf, 1.8f, card_w - 64.0f, COLOR_TEXT_WHITE);

    /* Stages row: Preflight -> Extract -> Compile -> Package */
    static const char *stages[] = { "Preflight", "Extract", "Compile", "Package" };
    static const PackageBuildStage stage_enums[] = {
        PACKAGE_BUILD_STAGE_PREFLIGHT,
        PACKAGE_BUILD_STAGE_EXTRACT,
        PACKAGE_BUILD_STAGE_COMPILE,
        PACKAGE_BUILD_STAGE_PACKAGE
    };
    float stage_x = card_x + 32.0f;
    float stage_y = card_y + 106.0f;
    float chip_w = (card_w - 64.0f - 30.0f) / 4.0f;
    if (chip_w > 150.0f) chip_w = 150.0f;

    for (int i = 0; i < 4; i++) {
        SDL_Color bg = (SDL_Color){ 20, 26, 32, 255 };
        SDL_Color text_col = COLOR_TEXT_MUTED;
        SDL_Color border_col = COLOR_CARD_BORDER;
        if (app->build_session.current_stage == stage_enums[i]) {
            bg = COLOR_BLUE;
            text_col = COLOR_TEXT_WHITE;
            border_col = COLOR_BLUE;
        } else if (app->build_session.current_stage > stage_enums[i] || app->build_session.is_complete) {
            text_col = COLOR_EMERALD;
            border_col = COLOR_EMERALD;
        }
        float cur_x = stage_x + (float)i * (chip_w + 10.0f);
        draw_rounded_fill(ren, cur_x, stage_y, chip_w, 28.0f, 6.0f, bg);
        draw_rounded_outline(ren, cur_x, stage_y, chip_w, 28.0f, 6.0f, border_col);
        float t_w = 0.0f, t_h = 0.0f;
        text_size_at_scale(stages[i], 1.0f, &t_w, &t_h);
        draw_text(ren, cur_x + (chip_w - t_w) * 0.5f, stage_y + (28.0f - t_h) * 0.5f,
                  stages[i], 1.0f, text_col);
    }

    /* Progress bar */
    float bar_y = stage_y + 40.0f;
    draw_indeterminate_bar(ren, card_x + 32.0f, bar_y, card_w - 64.0f, 16.0f, g_reduce_motion);

    /* Current status message & elapsed time */
    float info_y = bar_y + 24.0f;
    const char *msg = app->build_session.current_message[0]
        ? app->build_session.current_message : "Building package...";
    draw_text_ellipsized(ren, card_x + 32.0f, info_y, msg, 1.1f, card_w - 180.0f, COLOR_TEXT_WHITE);

    char elapsed_str[64];
    unsigned int sec = app->build_session.elapsed_ms / 1000;
    unsigned int tenths = (app->build_session.elapsed_ms % 1000) / 100;
    snprintf(elapsed_str, sizeof(elapsed_str), "Elapsed: %u.%u s", sec, tenths);
    draw_text(ren, card_x + card_w - 150.0f, info_y, elapsed_str, 1.0f, COLOR_TEXT_MUTED);

    /* Output log box */
    float log_y = info_y + 28.0f;
    float log_h = 100.0f;
    draw_rounded_fill(ren, card_x + 32.0f, log_y, card_w - 64.0f, log_h, 6.0f, (SDL_Color){ 14, 18, 22, 255 });
    draw_rounded_outline(ren, card_x + 32.0f, log_y, card_w - 64.0f, log_h, 6.0f, COLOR_CARD_BORDER);

    int count = app->build_session.output_line_count;
    int display_lines = count > 4 ? 4 : count;
    int start_idx = count > 4 ? count - 4 : 0;
    float text_y = log_y + 8.0f;
    for (int i = 0; i < display_lines; i++) {
        const char *l = package_builder_get_output_line(&app->build_session, start_idx + i);
        draw_text_ellipsized(ren, card_x + 44.0f, text_y, l, 0.95f, card_w - 88.0f, COLOR_TEXT_MUTED);
        text_y += 22.0f;
    }

    /* Cancel button */
    float btn_y = card_y + card_h - 58.0f;
    bool focused = (app->focus_index == 0);
    if (draw_button_focused(ren, card_x + 32.0f, btn_y, 180.0f, 44.0f, "CANCEL BUILD", false, in, focused)) {
        player_app_cancel_package_build(app);
        player_app_set_view(app, VIEW_LIBRARY);
    }
}

/* --- View: Error Dialog --- */
static void render_error(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float cx = w * 0.5f;
    float cy = h * 0.5f;
    float card_w = dialog_card_w(w, 680.0f);
    bool has_build_details = (app->last_error.failed_stage[0] != '\0' ||
                              app->last_error.log_file_path[0] != '\0');
    float card_h = has_build_details ? 420.0f : 340.0f;
    if (card_h > h - 32.0f && h > 340.0f) card_h = h - 32.0f;
    float card_x = centered_card_x(w, card_w);
    if (cx - card_w * 0.5f >= 16.0f) card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 60.0f) card_y = 60.0f;

    draw_shadow(ren, card_x, card_y, card_w, card_h, 10.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_RED);

    draw_badge(ren, card_x + 32.0f, card_y + 28.0f, app->last_error.error_code, COLOR_RED);
    draw_text_ellipsized(ren, card_x + 32.0f, card_y + 66.0f, app->last_error.title,
                         2.0f, card_w - 64.0f, COLOR_TEXT_WHITE);

    float text_y = card_y + 110.0f;
    if (app->last_error.failed_stage[0]) {
        char stage_str[128];
        snprintf(stage_str, sizeof(stage_str), "FAILED STAGE: %s", app->last_error.failed_stage);
        draw_badge(ren, card_x + 32.0f, text_y, stage_str, COLOR_AMBER);
        text_y += 34.0f;
    }

    const char *err_msg = app->last_error.boundary_text[0]
        ? app->last_error.boundary_text
        : app->last_error.message;
    draw_text_wrapped(ren, card_x + 32.0f, text_y, card_w - 64.0f,
                      err_msg, 1.1f, COLOR_TEXT_MUTED, 4);
    text_y += 76.0f;

    if (app->last_error.log_file_path[0]) {
        char log_str[NK_MAX_PATH + 32];
        snprintf(log_str, sizeof(log_str), "LOG FILE: %s", app->last_error.log_file_path);
        draw_text_ellipsized(ren, card_x + 32.0f, text_y, log_str, 0.95f, card_w - 64.0f, COLOR_TEXT_MUTED);
    }

    bool focused = (app->focus_index == 0);
    float btn_y = card_y + card_h - 58.0f;
    if (draw_button_focused(ren, card_x + 32.0f, btn_y, 240.0f, 48.0f, app->last_error.recovery_action_label, true, in, focused)) {
        player_app_set_view(app, app->last_error.return_view);
    }
}

/* --- View: Setup Wizard --- */
static void render_setup_wizard(SDL_Renderer *ren, PlayerApp *app, const UiInput *in) {
    float w = (float)app->window_width;
    float h = (float)app->window_height;
    float cx = w * 0.5f;
    float cy = h * 0.5f;

    float card_w = dialog_card_w(w, 800.0f);
    float card_h = 480.0f;
    if (card_h > h - 60.0f && h > 480.0f) card_h = h - 60.0f;
    float card_x = centered_card_x(w, card_w);
    if (cx - card_w * 0.5f >= 16.0f) card_x = cx - card_w * 0.5f;
    float card_y = cy - card_h * 0.5f;
    if (card_y < 70.0f) card_y = 70.0f;

    draw_shadow(ren, card_x, card_y, card_w, card_h, 12.0f);
    draw_rounded_fill(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BG);
    draw_rounded_outline(ren, card_x, card_y, card_w, card_h, 10.0f, COLOR_CARD_BORDER);

    /* Wizard Step Breadcrumbs */
    static const char *kSteps[] = { "1. Welcome", "2. Select Game", "3. Verify", "4. Fonts & System", "5. Ready" };
    float bc_x = card_x + 32.0f;
    float bc_y = card_y + 24.0f;
    float bc_step_w = (card_w - 64.0f) / 5.0f;
    for (int i = 0; i < 5; i++) {
        bool current = (i == (int)app->wizard.step);
        bool past = (i < (int)app->wizard.step);
        SDL_Color pill_color = current ? COLOR_EMERALD : (past ? COLOR_BLUE : COLOR_TEXT_DIM);
        if (card_w >= 660.0f) {
            draw_badge(ren, bc_x + (float)i * bc_step_w, bc_y, kSteps[i], pill_color);
        } else {
            char step_num[16];
            snprintf(step_num, sizeof(step_num), "STEP %d", i + 1);
            draw_badge(ren, bc_x + (float)i * bc_step_w, bc_y, step_num, pill_color);
        }
    }

    float content_y = card_y + 68.0f;
    float btn_y = card_y + card_h - 60.0f;
    int focus = 0;

    switch (app->wizard.step) {
        case WIZARD_STEP_WELCOME: {
            draw_text(ren, card_x + 32.0f, content_y, "First-Time User Setup Wizard", 2.0f, COLOR_TEXT_WHITE);
            draw_text_wrapped(ren, card_x + 32.0f, content_y + 40.0f, card_w - 64.0f,
                              "Welcome to Nakagawa Recomp!\n\n"
                              "This wizard inspects a PSP disc image and stages title data. Launch is available "
                              "only for catalogued titles with a generated runtime package.\n\n"
                              "What you will need:\n"
                              " • Lawfully obtained PSP game ISO\n"
                              " • Windows PC with a supported graphics driver\n"
                              " • Keyboard and mouse; a gamepad is optional\n\n"
                              "This build does not decrypt encrypted executables (#295). It builds runtime "
                              "packages from the library (#296/#297). Verify also lists font (#300) and audio "
                              "(#301) status.",
                              1.1f, COLOR_TEXT_MUTED, 8);

            bool get_started_foc = (app->focus_index == focus++);
            if (draw_button_focused(ren, card_x + 32.0f, btn_y, 200.0f, 44.0f, "GET STARTED", true, in, get_started_foc)) {
                player_app_wizard_next(app);
            }
            bool skip_foc = (app->focus_index == focus++);
            if (draw_button_focused(ren, card_x + 248.0f, btn_y, 160.0f, 44.0f, "SKIP TO LIBRARY", false, in, skip_foc)) {
                player_app_wizard_cancel(app);
            }
            break;
        }

        case WIZARD_STEP_SELECT_GAME: {
            draw_text(ren, card_x + 32.0f, content_y, "Select Game Disc Image", 2.0f, COLOR_TEXT_WHITE);
            draw_text_wrapped(ren, card_x + 32.0f, content_y + 36.0f, card_w - 64.0f,
                              "Select your lawfully obtained PSP game disc image (*.iso).\n"
                              "Nakagawa Recomp will inspect the ISO filesystem, volume descriptors, and PARAM.SFO.",
                              1.1f, COLOR_TEXT_MUTED, 3);

            float box_y = content_y + 90.0f;
            float box_h = 96.0f;
            draw_rounded_fill(ren, card_x + 32.0f, box_y, card_w - 64.0f, box_h, 8.0f, (SDL_Color){ 16, 21, 26, 255 });
            draw_rounded_outline(ren, card_x + 32.0f, box_y, card_w - 64.0f, box_h, 8.0f, COLOR_CARD_BORDER);

            if (app->inspecting_game.iso_path[0] != '\0') {
                draw_badge(ren, card_x + 48.0f, box_y + 12.0f, "SELECTED ISO", COLOR_EMERALD);
                char sz_str[48];
                format_size(sz_str, sizeof(sz_str), app->inspecting_game.iso_size_bytes);
                char detail_str[128];
                snprintf(detail_str, sizeof(detail_str), "Size: %s · Disc ID: %s", sz_str,
                         app->inspecting_game.disc_id[0] ? app->inspecting_game.disc_id : "Inspected");
                draw_text(ren, card_x + 180.0f, box_y + 14.0f, detail_str, 1.0f, COLOR_TEXT_DIM);
                draw_text_ellipsized(ren, card_x + 48.0f, box_y + 44.0f, app->inspecting_game.iso_path, 1.1f, card_w - 96.0f, COLOR_TEXT_WHITE);
                draw_text(ren, card_x + 48.0f, box_y + 70.0f, app->wizard.status_message[0] ? app->wizard.status_message : "Ready to verify title.", 1.0f, COLOR_EMERALD);
            } else {
                draw_badge(ren, card_x + 48.0f, box_y + 16.0f, "NO FILE CHOSEN", COLOR_AMBER);
                draw_text(ren, card_x + 48.0f, box_y + 50.0f, "Click 'Choose PSP Game ISO' or drag & drop an ISO file onto this window.", 1.1f, COLOR_TEXT_MUTED);
            }

            if (!app->wizard.iso_selected) {
                bool browse_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 32.0f, btn_y, 220.0f, 44.0f, "CHOOSE PSP GAME ISO", true, in, browse_foc)) {
                    app->request_file_picker = true;
                }
                bool back_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 268.0f, btn_y, 100.0f, 44.0f, "BACK", false, in, back_foc)) {
                    player_app_wizard_back(app);
                }
                bool cancel_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 384.0f, btn_y, 100.0f, 44.0f, "CANCEL", false, in, cancel_foc)) {
                    player_app_wizard_cancel(app);
                }
            } else {
                bool proceed_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 32.0f, btn_y, 190.0f, 44.0f, "PROCEED TO VERIFY", true, in, proceed_foc)) {
                    player_app_wizard_next(app);
                }
                bool diff_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 238.0f, btn_y, 200.0f, 44.0f, "CHOOSE DIFFERENT ISO", false, in, diff_foc)) {
                    app->request_file_picker = true;
                }
                bool back_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 454.0f, btn_y, 90.0f, 44.0f, "BACK", false, in, back_foc)) {
                    player_app_wizard_back(app);
                }
                bool cancel_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 560.0f, btn_y, 90.0f, 44.0f, "CANCEL", false, in, cancel_foc)) {
                    player_app_wizard_cancel(app);
                }
            }
            break;
        }

        case WIZARD_STEP_INSPECT_VERIFY: {
            bool experimental = app->inspecting_game.is_experimental;
            bool supported = !experimental &&
                             (app->wizard.extraction_complete ||
                              app->inspecting_game.status == NK_STATUS_VERIFIED);
            draw_text(ren, card_x + 32.0f, content_y,
                      app->wizard.is_extracting ? "Extracting Game Assets" : "Compatibility Preflight & Asset Staging",
                      2.0f, COLOR_TEXT_WHITE);

            float panel_y = content_y + 40.0f;
            float panel_h = app->wizard.is_extracting ? 190.0f : 244.0f;
            draw_rounded_fill(ren, card_x + 32.0f, panel_y, card_w - 64.0f, panel_h, 8.0f, (SDL_Color){ 16, 21, 26, 255 });
            draw_rounded_outline(ren, card_x + 32.0f, panel_y, card_w - 64.0f, panel_h, 8.0f,
                                 app->wizard.is_extracting ? COLOR_AMBER : (supported ? COLOR_EMERALD : COLOR_RED));

            const char *verification_badge = experimental ? "EXPERIMENTAL"
                : (supported ? "TITLE PROFILED" : "PROFILE MISSING");
            SDL_Color verification_color = experimental ? COLOR_AMBER
                : (supported ? COLOR_EMERALD : COLOR_RED);
            if (app->wizard.is_extracting) {
                verification_badge = "EXTRACTING ASSETS";
                verification_color = COLOR_AMBER;
            } else if (app->wizard.extraction_complete) {
                verification_badge = "ASSETS STAGED";
            } else if (app->wizard.extraction_failed) {
                verification_badge = "STAGING FAILED";
                verification_color = COLOR_RED;
            }
            draw_badge(ren, card_x + 48.0f, panel_y + 16.0f, verification_badge, verification_color);
            draw_badge(ren, card_x + 230.0f, panel_y + 16.0f, app->inspecting_game.disc_id[0] ? app->inspecting_game.disc_id : "UNKNOWN", COLOR_BLUE);

            draw_text_ellipsized(ren, card_x + 48.0f, panel_y + 52.0f,
                                 app->inspecting_game.title_name[0] ? app->inspecting_game.title_name : "PlayStation Portable Title",
                                 2.0f, card_w - 96.0f, COLOR_TEXT_WHITE);

            if (app->wizard.is_extracting) {
                draw_text(ren, card_x + 48.0f, panel_y + 76.0f,
                          "The ISO is being copied into an isolated local staging tree.",
                          1.0f, COLOR_TEXT_MUTED);
            } else if (experimental) {
                draw_text(ren, card_x + 48.0f, panel_y + 76.0f,
                          "Experimental: this game has not been verified. Compatibility is unknown.",
                          0.95f, COLOR_AMBER);
            } else if (supported) {
                draw_text(ren, card_x + 48.0f, panel_y + 76.0f,
                          "Disc identity is listed in the native title catalog.",
                          0.95f, COLOR_TEXT_MUTED);
            } else {
                draw_text(ren, card_x + 48.0f, panel_y + 76.0f,
                          "Disc identity is not listed in the native title catalog.",
                          0.95f, COLOR_AMBER);
            }

            if (!app->wizard.is_extracting) {
                for (size_t i = 0; i < app->wizard.preflight.count; i++) {
                    const PlayerPreflightCheck *check = &app->wizard.preflight.checks[i];
                    const char *status_text = "UNKNOWN";
                    SDL_Color status_color = COLOR_TEXT_DIM;
                    switch (check->status) {
                        case PREFLIGHT_OK:
                            status_text = "OK";
                            status_color = COLOR_EMERALD;
                            break;
                        case PREFLIGHT_MISSING:
                            status_text = "MISSING";
                            status_color = COLOR_AMBER;
                            break;
                        case PREFLIGHT_INCOMPATIBLE:
                            status_text = "INCOMPATIBLE";
                            status_color = COLOR_RED;
                            break;
                        case PREFLIGHT_STALE:
                            status_text = "STALE";
                            status_color = COLOR_AMBER;
                            break;
                        case PREFLIGHT_UNSUPPORTED:
                            status_text = "UNSUPPORTED";
                            status_color = COLOR_RED;
                            break;
                        case PREFLIGHT_IN_PROGRESS:
                            status_text = "IN PROGRESS";
                            status_color = COLOR_AMBER;
                            break;
                        case PREFLIGHT_INVALID:
                            status_text = "INVALID";
                            status_color = COLOR_RED;
                            break;
                    }
                    float row_y = panel_y + 100.0f + (float)i * 25.0f;
                    draw_text_ellipsized(ren, card_x + 48.0f, row_y,
                                         status_text, 0.8f, 80.0f, status_color);
                    draw_text_ellipsized(ren, card_x + 132.0f, row_y,
                                         check->code, 0.8f, 122.0f, COLOR_TEXT_WHITE);
                    float issue_width = (float)check->issue_count * 54.0f;
                    draw_text_ellipsized(ren, card_x + 260.0f, row_y,
                                         check->message, 0.78f, card_w - 308.0f - issue_width,
                                         COLOR_TEXT_MUTED);
                    if (check->issue_count) {
                        draw_issue_links(ren, card_x + card_w - 48.0f - issue_width,
                                         row_y - 2.0f, check->issue_numbers,
                                         check->issue_count, in);
                    }
                }
            }

            if (app->wizard.is_extracting) {
                char progress_text[96];
                snprintf(progress_text, sizeof(progress_text), "Extracted %d of %d files · %d%%",
                         app->wizard.files_extracted, app->wizard.total_files,
                         app->wizard.extraction_percent);
                draw_text(ren, card_x + 48.0f, panel_y + 100.0f, progress_text,
                          1.0f, COLOR_TEXT_WHITE);
                draw_progress_bar(ren, card_x + 48.0f, panel_y + 124.0f,
                                  card_w - 96.0f, 18.0f,
                                  (float)app->wizard.extraction_percent);
                draw_text_ellipsized(ren, card_x + 48.0f, panel_y + 154.0f,
                                     app->wizard.extraction_current_file[0]
                                         ? app->wizard.extraction_current_file
                                         : "Preparing ISO payload...",
                                     1.0f, card_w - 96.0f, COLOR_TEXT_DIM);

                bool cancel_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 32.0f, btn_y, 140.0f, 44.0f,
                                        "CANCEL EXTRACTION", false, in, cancel_foc)) {
                    player_app_wizard_cancel(app);
                }
            } else if (experimental) {
                bool add_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 32.0f, btn_y, 220.0f, 44.0f,
                                        "ADD TO LIBRARY", true, in, add_foc)) {
                    if (player_app_add_game(app, &app->inspecting_game)) {
                        player_app_set_view(app, VIEW_LIBRARY);
                    } else {
                        player_app_set_error(app, "LIBRARY_WRITE_FAILED", "Could Not Save to Library",
                                             "The experimental title could not be stored. The library may be full, or the user data directory is not writable.",
                                             "Return to Library", VIEW_LIBRARY);
                    }
                }
                bool diff_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 268.0f, btn_y, 200.0f, 44.0f,
                                        "CHOOSE DIFFERENT ISO", false, in, diff_foc)) {
                    app->wizard.step = WIZARD_STEP_SELECT_GAME;
                    app->request_file_picker = true;
                }
                bool cancel_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 484.0f, btn_y, 100.0f, 44.0f,
                                        "CANCEL", false, in, cancel_foc)) {
                    player_app_wizard_cancel(app);
                }
            } else if (supported) {
                const char *continue_label = app->wizard.extraction_complete
                    ? "CONTINUE TO SETTINGS"
                    : (app->wizard.extraction_failed ? "RETRY EXTRACTION" : "EXTRACT & CONTINUE");
                bool cont_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 32.0f, btn_y, 220.0f, 44.0f, continue_label, true, in, cont_foc)) {
                    player_app_wizard_next(app);
                }
                bool back_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 268.0f, btn_y, 100.0f, 44.0f, "BACK", false, in, back_foc)) {
                    player_app_wizard_back(app);
                }
                bool cancel_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 384.0f, btn_y, 100.0f, 44.0f, "CANCEL", false, in, cancel_foc)) {
                    player_app_wizard_cancel(app);
                }
            } else {
                bool diff_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 32.0f, btn_y, 220.0f, 44.0f, "CHOOSE DIFFERENT ISO", true, in, diff_foc)) {
                    app->wizard.step = WIZARD_STEP_SELECT_GAME;
                    app->request_file_picker = true;
                }
                bool back_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 268.0f, btn_y, 100.0f, 44.0f, "BACK", false, in, back_foc)) {
                    player_app_wizard_back(app);
                }
                bool cancel_foc = (app->focus_index == focus++);
                if (draw_button_focused(ren, card_x + 384.0f, btn_y, 100.0f, 44.0f, "CANCEL", false, in, cancel_foc)) {
                    player_app_wizard_cancel(app);
                }
            }
            break;
        }

        case WIZARD_STEP_SYSTEM_FONTS: {
            draw_text(ren, card_x + 32.0f, content_y, "System & Open-Source Typography", 2.0f, COLOR_TEXT_WHITE);

            float spec_y = content_y + 36.0f;
            float spec_h = 170.0f;
            draw_rounded_fill(ren, card_x + 32.0f, spec_y, card_w - 64.0f, spec_h, 8.0f, (SDL_Color){ 16, 21, 26, 255 });
            draw_rounded_outline(ren, card_x + 32.0f, spec_y, card_w - 64.0f, spec_h, 8.0f, COLOR_CARD_BORDER);

            char res_txt[64];
            snprintf(res_txt, sizeof(res_txt), "Internal Resolution: %s (%dx Native)",
                     resolution_label(app->settings.resolution_scale), app->settings.resolution_scale);
            draw_text(ren, card_x + 48.0f, spec_y + 14.0f, res_txt, 1.1f, COLOR_TEXT_WHITE);

            char ctrl_txt[96];
            snprintf(ctrl_txt, sizeof(ctrl_txt), "Controller: %s (%s)",
                     app->settings.controller_connected ? app->settings.controller_name : "Keyboard / Mouse",
                     app->settings.controller_connected ? "Connected" : "Ready");
            draw_text(ren, card_x + 48.0f, spec_y + 38.0f, ctrl_txt, 1.0f, COLOR_LIME);

            draw_text(ren, card_x + 48.0f, spec_y + 66.0f, "Open-Source Typography Defaults (SIL Open Font License):", 1.0f, COLOR_TEXT_DIM);
            draw_text(ren, card_x + 48.0f, spec_y + 88.0f,
                      " • In-Game UI: M PLUS Rounded 1c / Rubik defaults", 1.0f, COLOR_TEXT_MUTED);
            draw_text(ren, card_x + 48.0f, spec_y + 108.0f,
                      " • Native Menus & Setup: Inter / Roboto Flex / Rubik", 1.0f, COLOR_TEXT_MUTED);
            draw_text(ren, card_x + 48.0f, spec_y + 128.0f,
                      " • Japanese Kana/Kanji: Kosugi Maru / Zen Maru Gothic", 1.0f, COLOR_TEXT_MUTED);
            draw_text(ren, card_x + 48.0f, spec_y + 148.0f,
                      " • Optional user-owned PSP font (jpn0.pgf): Loaded from font/ if installed", 0.9f, COLOR_AMBER);

            bool accept_foc = (app->focus_index == focus++);
            if (draw_button_focused(ren, card_x + 32.0f, btn_y, 200.0f, 44.0f, "ACCEPT & CONTINUE", true, in, accept_foc)) {
                app->wizard.font_confirmed = true;
                player_app_wizard_next(app);
            }
            bool cycle_foc = (app->focus_index == focus++);
            if (draw_button_focused(ren, card_x + 248.0f, btn_y, 190.0f, 44.0f, "CYCLE RESOLUTION", false, in, cycle_foc)) {
                player_app_cycle_resolution_scale(app, 1);
            }
            bool back_foc = (app->focus_index == focus++);
            if (draw_button_focused(ren, card_x + 454.0f, btn_y, 90.0f, 44.0f, "BACK", false, in, back_foc)) {
                player_app_wizard_back(app);
            }
            bool cancel_foc = (app->focus_index == focus++);
            if (draw_button_focused(ren, card_x + 560.0f, btn_y, 90.0f, 44.0f, "CANCEL", false, in, cancel_foc)) {
                player_app_wizard_cancel(app);
            }
            break;
        }

        case WIZARD_STEP_READY_LAUNCH: {
            bool runtime_ready = app->inspecting_game.is_prepared;
            draw_text(ren, card_x + 32.0f, content_y,
                      runtime_ready ? "Setup Complete - Ready to Play!"
                                    : "Assets Staged - Runtime Preparation Required",
                      2.0f, COLOR_TEXT_WHITE);

            float rdy_y = content_y + 40.0f;
            float rdy_h = 160.0f;
            draw_rounded_fill(ren, card_x + 32.0f, rdy_y, card_w - 64.0f, rdy_h, 8.0f, (SDL_Color){ 16, 21, 26, 255 });
            draw_rounded_outline(ren, card_x + 32.0f, rdy_y, card_w - 64.0f, rdy_h, 8.0f,
                                 runtime_ready ? COLOR_EMERALD : COLOR_AMBER);

            draw_badge(ren, card_x + 48.0f, rdy_y + 16.0f,
                       runtime_ready ? "READY TO PLAY" : "ASSETS STAGED",
                       runtime_ready ? COLOR_EMERALD : COLOR_AMBER);
            if (app->inspecting_game.disc_id[0]) {
                draw_badge(ren, card_x + 200.0f, rdy_y + 16.0f, app->inspecting_game.disc_id, COLOR_BLUE);
            }
            draw_text_ellipsized(ren, card_x + 48.0f, rdy_y + 54.0f,
                                 app->inspecting_game.title_name[0] ? app->inspecting_game.title_name : "PlayStation Portable title",
                                 2.2f, card_w - 96.0f, COLOR_TEXT_WHITE);

            char sum_line[192];
            snprintf(sum_line, sizeof(sum_line), "Resolution: %s · Display: %s · Sound: %d%% · Runtime: %s",
                     resolution_label(app->settings.resolution_scale),
                     app->settings.fullscreen ? "Fullscreen" : "Windowed",
                     app->settings.master_volume,
                     runtime_ready ? "Ready" : "Pending");
            draw_text_ellipsized(ren, card_x + 48.0f, rdy_y + 96.0f, sum_line,
                                 1.0f, card_w - 96.0f, COLOR_TEXT_MUTED);
            draw_text_wrapped(ren, card_x + 48.0f, rdy_y + 124.0f,
                              card_w - 96.0f,
                              runtime_ready
                                  ? "Click 'Launch Game Now' to start immediately, or 'Go to Library' to view your game cards."
                                  : "The disc assets are saved locally. Complete runtime preparation before launching the game.",
                              1.0f, COLOR_TEXT_DIM, 2);

            bool launch_foc = (app->focus_index == focus++);
            if (draw_button_focused(ren, card_x + 32.0f, btn_y, 200.0f, 44.0f,
                                    runtime_ready ? "LAUNCH GAME NOW" : "SAVE TO LIBRARY",
                                    true, in, launch_foc)) {
                if (app->inspecting_game.disc_id[0] != '\0') {
                    player_app_add_game(app, &app->inspecting_game);
                    int idx = player_app_find_game_by_disc_id(app, app->inspecting_game.disc_id);
                    if (idx >= 0) {
                        app->selected_game_index = idx;
                        if (runtime_ready) player_app_launch_game(app, idx);
                    }
                }
                player_app_set_view(app, VIEW_LIBRARY);
            }
            bool lib_foc = (app->focus_index == focus++);
            if (draw_button_focused(ren, card_x + 248.0f, btn_y, 170.0f, 44.0f, "GO TO LIBRARY", false, in, lib_foc)) {
                if (app->inspecting_game.disc_id[0] != '\0') {
                    player_app_add_game(app, &app->inspecting_game);
                    int idx = player_app_find_game_by_disc_id(app, app->inspecting_game.disc_id);
                    if (idx >= 0) {
                        app->selected_game_index = idx;
                    }
                }
                player_app_wizard_cancel(app);
            }
            bool back_foc = (app->focus_index == focus++);
            if (draw_button_focused(ren, card_x + 434.0f, btn_y, 100.0f, 44.0f, "BACK", false, in, back_foc)) {
                player_app_wizard_back(app);
            }
            break;
        }

        default:
            break;
    }
}

/* Focus-stop counts. The order lives in player_app_focus_count (pure state,
 * unit-tested); this stays a thin wrapper so render order and focus order
 * cannot drift apart unnoticed. */
int ui_focus_count(const PlayerApp *app) {
    return player_app_focus_count(app);
}

/* --- Main Frame Render Function --- */
void ui_render_frame(SDL_Renderer *renderer, PlayerApp *app, const UiInput *input) {
    if (!renderer || !app) return;

    /* Clamp focus before drawing so a resize or library change can never
     * leave the ring on a control that no longer exists. */
    int focus_count = ui_focus_count(app);
    if (focus_count < 1) focus_count = 1;
    if (app->focus_index < 0) app->focus_index = 0;
    if (app->focus_index >= focus_count) app->focus_index = focus_count - 1;

    g_reduce_motion = app->settings.reduce_motion;
    ui_font_set_density(app->dpi_scale > 0.0f ? app->dpi_scale : 1.0f);

    /* Background Clear */
    set_draw_color(renderer, COLOR_BG);
    SDL_RenderClear(renderer);

    /* Render Topbar Header */
    render_topbar(renderer, app, input);

    /* View Routing */
    switch (app->active_view) {
        case VIEW_LIBRARY:
        case PLAYER_VIEW_READY_LIBRARY:
            if (app->game_count == 0) {
                render_empty_library(renderer, app, input);
            } else {
                render_loaded_library(renderer, app, input);
            }
            break;
        case VIEW_INSPECTING:
            render_inspecting(renderer, app, input);
            break;
        case VIEW_SUPPORTED_TITLE:
            render_supported_title(renderer, app, input);
            break;
        case VIEW_EXPERIMENTAL_TITLE:
            render_experimental_title(renderer, app, input);
            break;
        case VIEW_UNSUPPORTED_TITLE:
            render_unsupported_title(renderer, app, input);
            break;
        case VIEW_PREPARING:
            render_preparing(renderer, app, input);
            break;
        case VIEW_SETTINGS:
            render_settings(renderer, app, input);
            break;
        case VIEW_CONTROLLER_SETTINGS:
            render_controller_settings(renderer, app, input);
            break;
        case VIEW_ERROR:
            render_error(renderer, app, input);
            break;
        case VIEW_SETUP_WIZARD:
            render_setup_wizard(renderer, app, input);
            break;
        case VIEW_BUILDING_PACKAGE:
            render_building_package(renderer, app, input);
            break;
        default:
            render_empty_library(renderer, app, input);
            break;
    }

    SDL_RenderPresent(renderer);
}

bool ui_capture_screenshot(SDL_Renderer *renderer, const char *output_bmp_path) {
    if (!renderer || !output_bmp_path) return false;
    SDL_Surface *surf = SDL_RenderReadPixels(renderer, NULL);
    if (!surf) return false;
    bool ok = SDL_SaveBMP(surf, output_bmp_path);
    SDL_DestroySurface(surf);
    return ok;
}
