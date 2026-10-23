/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* SDL3_ttf text backend for nk_ui (lane C3b).

   SDL3_ttf is loaded at run time (SDL_LoadObject), as the player does today, so
   this file needs the SDL3_ttf headers to compile but no import library: a host
   without the shared library gets a failed nk_ui_ttf_create and falls back.
   Shaping goes through a custom TTF_TextEngine whose CreateText callback copies
   the draw operations SDL3_ttf computed with HarfBuzz (clusters, kerning and
   per-glyph font fallback); rasterising uses TTF_GetGlyphImageForIndex.

   Compiled only with NK_UI_WITH_SDL_TTF; otherwise every entry is a stub. */

#include "backend/nk_ui_sdlr.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char g_nk_ui_ttf_error[256] = "";

static void nk_ui_ttf_set_error(const char *message) {
    snprintf(g_nk_ui_ttf_error, sizeof(g_nk_ui_ttf_error), "%s",
             message != NULL ? message : "");
}

const char *nk_ui_ttf_last_error(void) {
    return g_nk_ui_ttf_error;
}

#if !defined(NK_UI_WITH_SDL_TTF)

bool nk_ui_ttf_available(void) {
    return false;
}

NkUiTtf *nk_ui_ttf_create(const char *const *library_paths, size_t count) {
    (void)library_paths;
    (void)count;
    nk_ui_ttf_set_error("built without SDL3_ttf headers (NK_UI_WITH_SDL_TTF not defined)");
    return NULL;
}

void nk_ui_ttf_destroy(NkUiTtf *ttf) {
    (void)ttf;
}

bool nk_ui_ttf_add_font(NkUiTtf *ttf, int font, const char *path) {
    (void)ttf;
    (void)font;
    (void)path;
    return false;
}

bool nk_ui_ttf_set_fallback(NkUiTtf *ttf, int font, int fallback) {
    (void)ttf;
    (void)font;
    (void)fallback;
    return false;
}

NkUiTextBackend nk_ui_ttf_backend(NkUiTtf *ttf) {
    NkUiTextBackend none;
    (void)ttf;
    memset(&none, 0, sizeof(none));
    return none;
}

#else /* NK_UI_WITH_SDL_TTF */

#include <SDL3/SDL.h>
#include <SDL3_ttf/SDL_ttf.h>
#include <SDL3_ttf/SDL_textengine.h>

#define NK_UI_TTF_MAX_FONTS 8
#define NK_UI_TTF_MAX_FALLBACKS 3
#define NK_UI_TTF_MAX_FACES (1 + NK_UI_TTF_MAX_FALLBACKS)
#define NK_UI_TTF_MAX_INSTANCES 32
#define NK_UI_TTF_MAX_PX 512
#define NK_UI_TTF_MAX_STRING (1u << 20)
#define NK_UI_TTF_MAX_FONT_FILE (64u * 1024u * 1024u)

/* Every SDL3_ttf entry point used, resolved from the shared library. */
#define NK_UI_TTF_FUNCTIONS(X) \
    X(bool, TTF_Init, (void)) \
    X(void, TTF_Quit, (void)) \
    X(TTF_Font *, TTF_OpenFontIO, (SDL_IOStream *, bool, float)) \
    X(void, TTF_CloseFont, (TTF_Font *)) \
    X(void, TTF_SetFontHinting, (TTF_Font *, TTF_HintingFlags)) \
    X(bool, TTF_AddFallbackFont, (TTF_Font *, TTF_Font *)) \
    X(void, TTF_ClearFallbackFonts, (TTF_Font *)) \
    X(int, TTF_GetFontHeight, (const TTF_Font *)) \
    X(int, TTF_GetFontAscent, (const TTF_Font *)) \
    X(bool, TTF_GetStringSize, (TTF_Font *, const char *, size_t, int *, int *)) \
    X(TTF_Text *, TTF_CreateText, (TTF_TextEngine *, TTF_Font *, const char *, size_t)) \
    X(bool, TTF_UpdateText, (TTF_Text *)) \
    X(void, TTF_DestroyText, (TTF_Text *)) \
    X(SDL_Surface *, TTF_GetGlyphImageForIndex, (TTF_Font *, Uint32, TTF_ImageType *))

typedef struct {
#define NK_UI_TTF_MEMBER(ret, name, args) ret (SDLCALL *name) args;
    NK_UI_TTF_FUNCTIONS(NK_UI_TTF_MEMBER)
#undef NK_UI_TTF_MEMBER
} NkUiTtfApi;

typedef struct {
    bool used;
    unsigned char *data;
    size_t size;
    int fallback[NK_UI_TTF_MAX_FALLBACKS];
    int fallback_count;
} NkUiTtfFile;

/* One pixel size of one font, with its fallback faces already linked. */
typedef struct {
    bool used;
    int font;
    int px;
    uint32_t stamp;
    TTF_Font *face[NK_UI_TTF_MAX_FACES];
    int face_count;
    int ascent;
    int height;
} NkUiTtfInstance;

struct NkUiTtf {
    SDL_SharedObject *library;
    NkUiTtfApi api;
    bool inited;
    NkUiTtfFile files[NK_UI_TTF_MAX_FONTS];
    NkUiTtfInstance instances[NK_UI_TTF_MAX_INSTANCES];
    uint32_t clock;
    TTF_TextEngine engine;
    TTF_DrawOperation *ops;   /* copy of the last laid-out text's operations */
    int op_count;
    int op_capacity;
    unsigned char *bitmap;    /* A8 scratch handed out by raster() */
    size_t bitmap_capacity;
};

bool nk_ui_ttf_available(void) {
    return true;
}

static bool nk_ui_ttf_load_symbols(NkUiTtf *ttf, SDL_SharedObject *library,
                                   char *error, size_t error_size) {
#define NK_UI_TTF_RESOLVE(ret, name, args) \
    ttf->api.name = (ret (SDLCALL *) args)SDL_LoadFunction(library, #name); \
    if (ttf->api.name == NULL) { \
        snprintf(error, error_size, "SDL3_ttf is missing %s", #name); \
        return false; \
    }
    NK_UI_TTF_FUNCTIONS(NK_UI_TTF_RESOLVE)
#undef NK_UI_TTF_RESOLVE
    return true;
}

static bool nk_ui_ttf_engine_create(void *userdata, TTF_Text *text) {
    NkUiTtf *ttf = (NkUiTtf *)userdata;
    const TTF_TextData *data = text->internal;
    int count = data->num_ops;
    if (count < 0) {
        return false;
    }
    if (count > ttf->op_capacity) {
        TTF_DrawOperation *grown = (TTF_DrawOperation *)realloc(
            ttf->ops, (size_t)count * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        ttf->ops = grown;
        ttf->op_capacity = count;
    }
    if (count > 0) {
        memcpy(ttf->ops, data->ops, (size_t)count * sizeof(*ttf->ops));
    }
    ttf->op_count = count;
    ((TTF_Text *)text)->internal->engine_text = ttf;
    return true;
}

static void nk_ui_ttf_engine_destroy(void *userdata, TTF_Text *text) {
    (void)userdata;
    (void)text;
}

static void nk_ui_ttf_close_instance(NkUiTtf *ttf, NkUiTtfInstance *instance) {
    int i;
    if (!instance->used) {
        return;
    }
    ttf->api.TTF_ClearFallbackFonts(instance->face[0]);
    for (i = 0; i < instance->face_count; ++i) {
        ttf->api.TTF_CloseFont(instance->face[i]);
    }
    memset(instance, 0, sizeof(*instance));
}

static TTF_Font *nk_ui_ttf_open_face(NkUiTtf *ttf, const NkUiTtfFile *file, int px) {
    SDL_IOStream *stream = SDL_IOFromConstMem(file->data, file->size);
    TTF_Font *face;
    if (stream == NULL) {
        return NULL;
    }
    face = ttf->api.TTF_OpenFontIO(stream, true, (float)px);
    if (face != NULL) {
        ttf->api.TTF_SetFontHinting(face, TTF_HINTING_LIGHT);
    }
    return face;
}

static NkUiTtfInstance *nk_ui_ttf_instance(NkUiTtf *ttf, int font, int px) {
    NkUiTtfInstance *slot = NULL;
    NkUiTtfInstance *instance;
    const NkUiTtfFile *file;
    int i;
    if (ttf == NULL || font < 0 || font >= NK_UI_TTF_MAX_FONTS ||
        !ttf->files[font].used || px < 1 || px > NK_UI_TTF_MAX_PX) {
        return NULL;
    }
    for (i = 0; i < NK_UI_TTF_MAX_INSTANCES; ++i) {
        NkUiTtfInstance *candidate = &ttf->instances[i];
        if (candidate->used && candidate->font == font && candidate->px == px) {
            candidate->stamp = ++ttf->clock;
            return candidate;
        }
    }
    for (i = 0; i < NK_UI_TTF_MAX_INSTANCES; ++i) {
        NkUiTtfInstance *candidate = &ttf->instances[i];
        if (!candidate->used) {
            slot = candidate;
            break;
        }
        if (slot == NULL || candidate->stamp < slot->stamp) {
            slot = candidate;
        }
    }
    nk_ui_ttf_close_instance(ttf, slot);
    instance = slot;
    file = &ttf->files[font];
    instance->face[0] = nk_ui_ttf_open_face(ttf, file, px);
    if (instance->face[0] == NULL) {
        memset(instance, 0, sizeof(*instance));
        return NULL;
    }
    instance->face_count = 1;
    instance->used = true;
    instance->font = font;
    instance->px = px;
    for (i = 0; i < file->fallback_count; ++i) {
        TTF_Font *face = nk_ui_ttf_open_face(ttf, &ttf->files[file->fallback[i]], px);
        if (face == NULL) {
            continue;
        }
        if (!ttf->api.TTF_AddFallbackFont(instance->face[0], face)) {
            ttf->api.TTF_CloseFont(face);
            continue;
        }
        instance->face[instance->face_count++] = face;
    }
    instance->ascent = ttf->api.TTF_GetFontAscent(instance->face[0]);
    instance->height = ttf->api.TTF_GetFontHeight(instance->face[0]);
    instance->stamp = ++ttf->clock;
    return instance;
}

static NkUiTextSize nk_ui_ttf_measure(void *user, int font, int px,
                                      const char *text, size_t length) {
    NkUiTtf *ttf = (NkUiTtf *)user;
    NkUiTextSize size = {0.0f, 0.0f, 0.0f};
    NkUiTtfInstance *instance = nk_ui_ttf_instance(ttf, font, px);
    int width = 0;
    int height = 0;
    if (instance == NULL) {
        return size;
    }
    size.height = (float)instance->height;
    size.baseline = (float)instance->ascent;
    /* A length of 0 means "NUL-terminated" to SDL3_ttf, so empty is handled here. */
    if (text != NULL && length > 0 && length <= NK_UI_TTF_MAX_STRING &&
        ttf->api.TTF_GetStringSize(instance->face[0], text, length, &width, &height) &&
        width > 0) {
        size.width = (float)width;
    }
    return size;
}

static uint32_t nk_ui_ttf_decode(const char *text, size_t length, size_t at) {
    const unsigned char *s = (const unsigned char *)text;
    unsigned lead;
    size_t need;
    uint32_t cp;
    size_t i;
    if (at >= length) {
        return 0u;
    }
    lead = s[at];
    if (lead < 0x80u) {
        return lead;
    }
    if ((lead & 0xE0u) == 0xC0u) {
        need = 1u;
        cp = lead & 0x1Fu;
    } else if ((lead & 0xF0u) == 0xE0u) {
        need = 2u;
        cp = lead & 0x0Fu;
    } else if ((lead & 0xF8u) == 0xF0u) {
        need = 3u;
        cp = lead & 0x07u;
    } else {
        return 0xFFFDu;
    }
    if (length - at <= need) {
        return 0xFFFDu;
    }
    for (i = 1; i <= need; ++i) {
        if ((s[at + i] & 0xC0u) != 0x80u) {
            return 0xFFFDu;
        }
        cp = (cp << 6) | (s[at + i] & 0x3Fu);
    }
    return cp;
}

static int nk_ui_ttf_face_depth(const NkUiTtfInstance *instance, const TTF_Font *face) {
    int i;
    for (i = 0; i < instance->face_count; ++i) {
        if (instance->face[i] == face) {
            return i;
        }
    }
    return -1;
}

static int nk_ui_ttf_shape(void *user, int font, int px, const char *text, size_t length,
                           NkUiGlyph *out, int max) {
    NkUiTtf *ttf = (NkUiTtf *)user;
    NkUiTtfInstance *instance;
    TTF_Text *laid_out;
    int count = 0;
    int i;
    int total_width = 0;
    if (ttf == NULL || text == NULL || out == NULL || max <= 0 || length == 0 ||
        length > NK_UI_TTF_MAX_STRING) {
        return 0;
    }
    instance = nk_ui_ttf_instance(ttf, font, px);
    if (instance == NULL) {
        return 0;
    }
    ttf->op_count = 0;
    laid_out = ttf->api.TTF_CreateText(&ttf->engine, instance->face[0], text, length);
    if (laid_out == NULL) {
        return 0;
    }
    /* UpdateText lays the text out and calls nk_ui_ttf_engine_create. */
    if (ttf->api.TTF_UpdateText(laid_out)) {
        total_width = laid_out->internal != NULL ? laid_out->internal->w : 0;
    } else {
        ttf->op_count = 0;
    }
    for (i = 0; i < ttf->op_count && count < max; ++i) {
        const TTF_CopyOperation *op;
        int depth;
        if (ttf->ops[i].cmd != TTF_DRAW_COMMAND_COPY) {
            continue;
        }
        op = &ttf->ops[i].copy;
        depth = nk_ui_ttf_face_depth(instance, op->glyph_font);
        if (depth < 0 || op->dst.w <= 0 || op->dst.h <= 0 ||
            op->glyph_index > 0x00FFFFFFu) {
            continue;
        }
        out[count].glyph = ((uint32_t)depth << 24) | op->glyph_index;
        out[count].codepoint = nk_ui_ttf_decode(
            text, length, op->text_offset >= 0 ? (size_t)op->text_offset : length);
        out[count].x = (float)op->dst.x;
        out[count].y = (float)(op->dst.y + op->dst.h - instance->ascent);
        out[count].advance_x = 0.0f;
        out[count].advance_y = 0.0f;
        ++count;
    }
    for (i = 0; i < count; ++i) {
        float next = i + 1 < count ? out[i + 1].x : (float)total_width;
        out[i].advance_x = next > out[i].x ? next - out[i].x : 0.0f;
    }
    ttf->api.TTF_DestroyText(laid_out);
    return count;
}

static bool nk_ui_ttf_raster(void *user, int font, int px, uint32_t glyph,
                             NkUiGlyphBitmap *out) {
    NkUiTtf *ttf = (NkUiTtf *)user;
    NkUiTtfInstance *instance;
    SDL_Surface *image;
    SDL_Surface *argb = NULL;
    TTF_ImageType type = TTF_IMAGE_INVALID;
    size_t depth = (size_t)(glyph >> 24);
    size_t bytes;
    int x;
    int y;
    if (out == NULL) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    instance = nk_ui_ttf_instance(ttf, font, px);
    if (instance == NULL || depth >= (size_t)instance->face_count) {
        return false;
    }
    image = ttf->api.TTF_GetGlyphImageForIndex(instance->face[depth],
                                               glyph & 0x00FFFFFFu, &type);
    if (image == NULL || image->w <= 0 || image->h <= 0) {
        if (image != NULL) {
            SDL_DestroySurface(image);
        }
        return false;
    }
    if (image->format != SDL_PIXELFORMAT_ARGB8888) {
        argb = SDL_ConvertSurface(image, SDL_PIXELFORMAT_ARGB8888);
        SDL_DestroySurface(image);
        image = argb;
        if (image == NULL) {
            return false;
        }
    }
    bytes = (size_t)image->w * (size_t)image->h;
    if (bytes > ttf->bitmap_capacity) {
        unsigned char *grown = (unsigned char *)realloc(ttf->bitmap, bytes);
        if (grown == NULL) {
            SDL_DestroySurface(image);
            return false;
        }
        ttf->bitmap = grown;
        ttf->bitmap_capacity = bytes;
    }
    for (y = 0; y < image->h; ++y) {
        const Uint32 *row = (const Uint32 *)((const unsigned char *)image->pixels +
                                             (size_t)y * (size_t)image->pitch);
        for (x = 0; x < image->w; ++x) {
            ttf->bitmap[(size_t)y * (size_t)image->w + (size_t)x] =
                (unsigned char)(row[x] >> 24);
        }
    }
    out->pixels = ttf->bitmap;
    out->width = image->w;
    out->height = image->h;
    out->pitch = image->w;
    out->bearing_x = 0;
    out->bearing_y = image->h;
    out->advance_x = (float)image->w;
    out->advance_y = 0.0f;
    SDL_DestroySurface(image);
    return true;
}

static void nk_ui_ttf_clear_library(NkUiTtf *ttf) {
    if (ttf->library != NULL) {
        SDL_UnloadObject(ttf->library);
        ttf->library = NULL;
    }
    memset(&ttf->api, 0, sizeof(ttf->api));
}

NkUiTtf *nk_ui_ttf_create(const char *const *library_paths, size_t count) {
#if defined(_WIN32)
    static const char *const default_name = "SDL3_ttf.dll";
#elif defined(__APPLE__)
    static const char *const default_name = "libSDL3_ttf.0.dylib";
#else
    static const char *const default_name = "libSDL3_ttf.so.0";
#endif
    NkUiTtf *ttf;
    size_t i;
    char reason[200] = "";
    nk_ui_ttf_set_error("");
    ttf = (NkUiTtf *)calloc(1, sizeof(*ttf));
    if (ttf == NULL) {
        nk_ui_ttf_set_error("out of memory");
        return NULL;
    }
    for (i = 0; i <= count && ttf->library == NULL; ++i) {
        const char *candidate = i < count && library_paths != NULL ?
                                library_paths[i] : default_name;
        SDL_SharedObject *library;
        if (candidate == NULL || candidate[0] == '\0') {
            continue;
        }
        library = SDL_LoadObject(candidate);
        if (library == NULL) {
            snprintf(reason, sizeof(reason), "cannot load %s: %s", candidate, SDL_GetError());
            continue;
        }
        if (!nk_ui_ttf_load_symbols(ttf, library, reason, sizeof(reason))) {
            SDL_UnloadObject(library);
            continue;
        }
        ttf->library = library;
    }
    if (ttf->library == NULL) {
        nk_ui_ttf_set_error(reason[0] != '\0' ? reason : "SDL3_ttf library not found");
        free(ttf);
        return NULL;
    }
    if (!ttf->api.TTF_Init()) {
        snprintf(reason, sizeof(reason), "TTF_Init failed: %s", SDL_GetError());
        nk_ui_ttf_set_error(reason);
        nk_ui_ttf_clear_library(ttf);
        free(ttf);
        return NULL;
    }
    ttf->inited = true;
    SDL_INIT_INTERFACE(&ttf->engine);
    ttf->engine.userdata = ttf;
    ttf->engine.CreateText = nk_ui_ttf_engine_create;
    ttf->engine.DestroyText = nk_ui_ttf_engine_destroy;
    return ttf;
}

void nk_ui_ttf_destroy(NkUiTtf *ttf) {
    int i;
    if (ttf == NULL) {
        return;
    }
    for (i = 0; i < NK_UI_TTF_MAX_INSTANCES; ++i) {
        nk_ui_ttf_close_instance(ttf, &ttf->instances[i]);
    }
    for (i = 0; i < NK_UI_TTF_MAX_FONTS; ++i) {
        free(ttf->files[i].data);
    }
    if (ttf->inited) {
        ttf->api.TTF_Quit();
    }
    nk_ui_ttf_clear_library(ttf);
    free(ttf->ops);
    free(ttf->bitmap);
    free(ttf);
}

bool nk_ui_ttf_add_font(NkUiTtf *ttf, int font, const char *path) {
    size_t size = 0;
    void *data;
    if (ttf == NULL || path == NULL || font < 0 || font >= NK_UI_TTF_MAX_FONTS ||
        ttf->files[font].used) {
        return false;
    }
    data = SDL_LoadFile(path, &size);
    if (data == NULL || size == 0 || size > NK_UI_TTF_MAX_FONT_FILE) {
        char reason[200];
        snprintf(reason, sizeof(reason), "cannot read font %s", path);
        nk_ui_ttf_set_error(reason);
        SDL_free(data);
        return false;
    }
    /* SDL_LoadFile memory belongs to SDL; keep a malloc copy so free() is valid. */
    ttf->files[font].data = (unsigned char *)malloc(size);
    if (ttf->files[font].data == NULL) {
        SDL_free(data);
        return false;
    }
    memcpy(ttf->files[font].data, data, size);
    SDL_free(data);
    ttf->files[font].size = size;
    ttf->files[font].used = true;
    return true;
}

bool nk_ui_ttf_set_fallback(NkUiTtf *ttf, int font, int fallback) {
    NkUiTtfFile *file;
    int i;
    if (ttf == NULL || font < 0 || font >= NK_UI_TTF_MAX_FONTS || fallback < 0 ||
        fallback >= NK_UI_TTF_MAX_FONTS || font == fallback ||
        !ttf->files[font].used || !ttf->files[fallback].used) {
        return false;
    }
    file = &ttf->files[font];
    if (file->fallback_count >= NK_UI_TTF_MAX_FALLBACKS) {
        return false;
    }
    for (i = 0; i < file->fallback_count; ++i) {
        if (file->fallback[i] == fallback) {
            return true;
        }
    }
    file->fallback[file->fallback_count++] = fallback;
    /* Instances already built keep their old chain; drop them so the next use
       builds the new one. */
    for (i = 0; i < NK_UI_TTF_MAX_INSTANCES; ++i) {
        if (ttf->instances[i].used && ttf->instances[i].font == font) {
            nk_ui_ttf_close_instance(ttf, &ttf->instances[i]);
        }
    }
    return true;
}

NkUiTextBackend nk_ui_ttf_backend(NkUiTtf *ttf) {
    NkUiTextBackend backend;
    memset(&backend, 0, sizeof(backend));
    backend.user = ttf;
    backend.measure = nk_ui_ttf_measure;
    backend.shape = nk_ui_ttf_shape;
    backend.raster = nk_ui_ttf_raster;
    return backend;
}

#endif /* NK_UI_WITH_SDL_TTF */
