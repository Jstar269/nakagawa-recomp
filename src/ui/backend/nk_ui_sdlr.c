/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* SDL_Renderer backend for nk_ui (lane C3b).

   One draw list, drawn with SDL_RenderGeometryRaw and the premultiplied blend
   mode. Rectangles, borders and rounded-corner fans are the list's own
   triangles. Text commands are shaped and rasterised here through the host's
   NkUiTextBackend into a per-scale glyph atlas: pages of 1024 x 1024 premultiplied
   white RGBA (the "A8 expanded" form the architecture calls for, since
   SDL_Renderer has no single-channel texture). Scissors become the renderer clip
   rectangle, intersected when nested. Image commands (NK_UI_CUSTOM_IMAGE) draw a
   texture uploaded once per NkUiImage.serial (and per drawn size when the image is
   shrunk, which is area-averaged on the CPU first, because SDL_Renderer has no
   mipmaps) and kept in a small least recently used cache.

   Compiled only with NK_UI_WITH_SDL; otherwise every entry is a stub. */

#include "nk_ui_sdlr.h"

#include <stdlib.h>
#include <string.h>

#if !defined(NK_UI_WITH_SDL)

bool nk_ui_sdlr_available(void) {
    return false;
}

NkUiSdlr *nk_ui_sdlr_create(struct SDL_Renderer *renderer, const NkUiTextBackend *text) {
    (void)renderer;
    (void)text;
    return NULL;
}

void nk_ui_sdlr_destroy(NkUiSdlr *renderer) {
    (void)renderer;
}

void nk_ui_sdlr_set_scale(NkUiSdlr *renderer, float density) {
    (void)renderer;
    (void)density;
}

bool nk_ui_sdlr_render(NkUiSdlr *renderer, const NkUiDrawList *list) {
    (void)renderer;
    (void)list;
    return false;
}

void nk_ui_sdlr_get_stats(const NkUiSdlr *renderer, NkUiSdlrStats *out) {
    (void)renderer;
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
}

#else /* NK_UI_WITH_SDL */

#include <SDL3/SDL.h>

#include "nk_ui_waves.h"

#include <limits.h>
#include <math.h>

#define NK_UI_SDLR_PAGE_SIZE 1024
#define NK_UI_SDLR_MAX_PAGES 8
#define NK_UI_SDLR_PAD 1
#define NK_UI_SDLR_CLIP_DEPTH 32
#define NK_UI_SDLR_MAX_GLYPHS_PER_RUN 8192
/* The waves are soft, so they render on the CPU at a third of the element's
   size (at most 480 px wide) and are scaled up with a linear filter. */
#define NK_UI_SDLR_WAVES_DIVISOR 3
#define NK_UI_SDLR_WAVES_MAX_W 480
/* Image textures kept alive: a library page of disc art, the title's picture
   and every prompt glyph set fit with room to spare. */
#define NK_UI_SDLR_IMAGE_CACHE 64
/* Larger images are left out (nk_ui decoders cap them far lower). */
#define NK_UI_SDLR_IMAGE_MAX_SIDE 8192

typedef struct {
    int font;
    int px;
    uint32_t glyph;
    int page;       /* -1: cached as empty or failed, nothing to draw */
    int x;
    int y;
    int w;
    int h;
    int bearing_x;
    int bearing_y;
} NkUiSdlrEntry;

typedef struct {
    SDL_Texture *texture;
    int cursor_x;
    int cursor_y;
    int row_height;
} NkUiSdlrPage;

typedef struct {
    float x;
    float y;
    SDL_FColor color;
    float u;
    float v;
} NkUiSdlrQuadVertex;

typedef struct {
    uint64_t serial;      /* 0: free */
    int width;            /* the texture's size: the image's own, or a shrunk copy's */
    int height;
    uint64_t last_used;   /* render count when last drawn */
    SDL_Texture *texture;
} NkUiSdlrImageEntry;

struct NkUiSdlr {
    SDL_Renderer *renderer;
    NkUiTextBackend text;
    int page_size;
    float density;
    NkUiSdlrPage pages[NK_UI_SDLR_MAX_PAGES];
    int page_count;
    NkUiSdlrEntry *entries;
    size_t entry_count;
    size_t entry_capacity;
    int32_t *table;
    size_t table_capacity;
    SDL_FColor *colors;       /* per-render conversion of the list's vertex colours */
    size_t color_capacity;
    NkUiGlyph *glyphs;
    size_t glyph_capacity;
    NkUiSdlrQuadVertex *quad_vertices;
    int *quad_indices;
    size_t quad_capacity;     /* glyphs the two arrays hold */
    uint32_t *upload;
    size_t upload_capacity;
    SDL_Texture *waves_texture;
    uint8_t *waves_pixels;
    int waves_w;
    int waves_h;
    bool waves_valid;
    NkUiWavesCommand waves_last;
    NkUiSdlrImageEntry images[NK_UI_SDLR_IMAGE_CACHE];
    uint64_t render_count;
    NkUiSdlrStats stats;
};

bool nk_ui_sdlr_available(void) {
    return true;
}

/* ---- atlas ---- */

static uint32_t nk_ui_sdlr_hash(int font, int px, uint32_t glyph) {
    uint32_t h = 2166136261u;
    h = (h ^ (uint32_t)font) * 16777619u;
    h = (h ^ (uint32_t)px) * 16777619u;
    h = (h ^ glyph) * 16777619u;
    h ^= h >> 15;
    return h;
}

static void nk_ui_sdlr_drop_atlas(NkUiSdlr *r) {
    int i;
    for (i = 0; i < r->page_count; ++i) {
        if (r->pages[i].texture != NULL) {
            SDL_DestroyTexture(r->pages[i].texture);
        }
    }
    memset(r->pages, 0, sizeof(r->pages));
    r->page_count = 0;
    r->entry_count = 0;
    if (r->table != NULL) {
        size_t i2;
        for (i2 = 0; i2 < r->table_capacity; ++i2) {
            r->table[i2] = -1;
        }
    }
}

static bool nk_ui_sdlr_table_grow(NkUiSdlr *r) {
    size_t capacity = r->table_capacity == 0 ? 512u : r->table_capacity * 2u;
    int32_t *table = (int32_t *)malloc(capacity * sizeof(*table));
    size_t i;
    if (table == NULL) {
        return false;
    }
    for (i = 0; i < capacity; ++i) {
        table[i] = -1;
    }
    for (i = 0; i < r->entry_count; ++i) {
        size_t slot = nk_ui_sdlr_hash(r->entries[i].font, r->entries[i].px,
                                      r->entries[i].glyph) & (capacity - 1u);
        while (table[slot] >= 0) {
            slot = (slot + 1u) & (capacity - 1u);
        }
        table[slot] = (int32_t)i;
    }
    free(r->table);
    r->table = table;
    r->table_capacity = capacity;
    return true;
}

static NkUiSdlrEntry *nk_ui_sdlr_find(NkUiSdlr *r, int font, int px, uint32_t glyph) {
    size_t slot;
    if (r->table_capacity == 0) {
        return NULL;
    }
    slot = nk_ui_sdlr_hash(font, px, glyph) & (r->table_capacity - 1u);
    while (r->table[slot] >= 0) {
        NkUiSdlrEntry *entry = &r->entries[r->table[slot]];
        if (entry->font == font && entry->px == px && entry->glyph == glyph) {
            return entry;
        }
        slot = (slot + 1u) & (r->table_capacity - 1u);
    }
    return NULL;
}

static NkUiSdlrEntry *nk_ui_sdlr_add_entry(NkUiSdlr *r, int font, int px, uint32_t glyph) {
    NkUiSdlrEntry *entry;
    size_t slot;
    if ((r->entry_count + 1u) * 2u > r->table_capacity && !nk_ui_sdlr_table_grow(r)) {
        return NULL;
    }
    if (r->entry_count == r->entry_capacity) {
        size_t capacity = r->entry_capacity == 0 ? 256u : r->entry_capacity * 2u;
        NkUiSdlrEntry *grown = (NkUiSdlrEntry *)realloc(r->entries,
                                                        capacity * sizeof(*grown));
        if (grown == NULL) {
            return NULL;
        }
        r->entries = grown;
        r->entry_capacity = capacity;
    }
    entry = &r->entries[r->entry_count];
    memset(entry, 0, sizeof(*entry));
    entry->font = font;
    entry->px = px;
    entry->glyph = glyph;
    entry->page = -1;
    slot = nk_ui_sdlr_hash(font, px, glyph) & (r->table_capacity - 1u);
    while (r->table[slot] >= 0) {
        slot = (slot + 1u) & (r->table_capacity - 1u);
    }
    r->table[slot] = (int32_t)r->entry_count;
    ++r->entry_count;
    return entry;
}

/* Finds room for a w x h cell (padding included) and returns its page, or -1. A
   new page is added when the current one is full; at the page limit the caller
   must rebuild the atlas. */
static int nk_ui_sdlr_place(NkUiSdlr *r, int w, int h, int *x, int *y) {
    int cell_w = w + NK_UI_SDLR_PAD;
    int cell_h = h + NK_UI_SDLR_PAD;
    int attempt;
    if (cell_w > r->page_size || cell_h > r->page_size) {
        return -1;
    }
    for (attempt = 0; attempt < 2; ++attempt) {
        if (r->page_count > 0) {
            NkUiSdlrPage *page = &r->pages[r->page_count - 1];
            if (page->cursor_x + cell_w > r->page_size) {
                page->cursor_x = 0;
                page->cursor_y += page->row_height;
                page->row_height = 0;
            }
            if (page->cursor_y + cell_h <= r->page_size) {
                *x = page->cursor_x;
                *y = page->cursor_y;
                page->cursor_x += cell_w;
                if (cell_h > page->row_height) {
                    page->row_height = cell_h;
                }
                return r->page_count - 1;
            }
        }
        if (r->page_count == NK_UI_SDLR_MAX_PAGES) {
            return -2;
        }
        {
            NkUiSdlrPage *page = &r->pages[r->page_count];
            page->texture = SDL_CreateTexture(r->renderer, SDL_PIXELFORMAT_RGBA32,
                                              SDL_TEXTUREACCESS_STATIC,
                                              r->page_size, r->page_size);
            if (page->texture == NULL) {
                return -1;
            }
            SDL_SetTextureBlendMode(page->texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
            SDL_SetTextureScaleMode(page->texture, SDL_SCALEMODE_NEAREST);
            page->cursor_x = 0;
            page->cursor_y = 0;
            page->row_height = 0;
            ++r->page_count;
        }
    }
    return -1;
}

static bool nk_ui_sdlr_upload(NkUiSdlr *r, int page, int x, int y,
                              const NkUiGlyphBitmap *bitmap) {
    size_t count = (size_t)bitmap->width * (size_t)bitmap->height;
    SDL_Rect rect;
    int row;
    int col;
    if (count > r->upload_capacity) {
        uint32_t *grown = (uint32_t *)realloc(r->upload, count * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        r->upload = grown;
        r->upload_capacity = count;
    }
    for (row = 0; row < bitmap->height; ++row) {
        const uint8_t *source = bitmap->pixels + (size_t)row * (size_t)bitmap->pitch;
        for (col = 0; col < bitmap->width; ++col) {
            uint32_t a = source[col];
            /* Premultiplied white: every channel equals the coverage. */
            r->upload[(size_t)row * (size_t)bitmap->width + (size_t)col] =
                a | (a << 8) | (a << 16) | (a << 24);
        }
    }
    rect.x = x;
    rect.y = y;
    rect.w = bitmap->width;
    rect.h = bitmap->height;
    if (!SDL_UpdateTexture(r->pages[page].texture, &rect, r->upload,
                           bitmap->width * (int)sizeof(uint32_t))) {
        return false;
    }
    ++r->stats.atlas_uploads;
    return true;
}

static void nk_ui_sdlr_flush_quads(NkUiSdlr *r, int page, int quads);

/* Returns the cached entry for a glyph, rasterising and uploading it on first
   use. `pending_page` and `pending_quads` describe geometry the caller has built
   but not drawn; it is flushed before an atlas rebuild invalidates its texture.
   Returns NULL only on allocation or backend failure. */
static NkUiSdlrEntry *nk_ui_sdlr_glyph(NkUiSdlr *r, int font, int px, uint32_t glyph,
                                       int *pending_page, int *pending_quads) {
    NkUiSdlrEntry *entry = nk_ui_sdlr_find(r, font, px, glyph);
    NkUiGlyphBitmap bitmap;
    int page;
    int x = 0;
    int y = 0;
    if (entry != NULL) {
        return entry;
    }
    memset(&bitmap, 0, sizeof(bitmap));
    if (r->text.raster == NULL ||
        !r->text.raster(r->text.user, font, px, glyph, &bitmap) ||
        bitmap.pixels == NULL || bitmap.width <= 0 || bitmap.height <= 0 ||
        bitmap.pitch < bitmap.width) {
        /* Blank glyphs and failures are remembered, so they cost one call. */
        return nk_ui_sdlr_add_entry(r, font, px, glyph);
    }
    page = nk_ui_sdlr_place(r, bitmap.width, bitmap.height, &x, &y);
    if (page == -2) {
        /* Atlas full: draw what is queued, drop every page, start again. */
        nk_ui_sdlr_flush_quads(r, *pending_page, *pending_quads);
        *pending_quads = 0;
        ++r->stats.atlas_rebuilds;
        nk_ui_sdlr_drop_atlas(r);
        page = nk_ui_sdlr_place(r, bitmap.width, bitmap.height, &x, &y);
        /* The raster result may have been overwritten by the flush's text calls
           only if the backend shares scratch; flush draws no text, so it holds. */
    }
    entry = nk_ui_sdlr_add_entry(r, font, px, glyph);
    if (entry == NULL) {
        return NULL;
    }
    if (page < 0 || !nk_ui_sdlr_upload(r, page, x, y, &bitmap)) {
        return entry; /* too large for a page or upload failed: skipped, cached */
    }
    entry->page = page;
    entry->x = x;
    entry->y = y;
    entry->w = bitmap.width;
    entry->h = bitmap.height;
    entry->bearing_x = bitmap.bearing_x;
    entry->bearing_y = bitmap.bearing_y;
    return entry;
}

/* ---- drawing ---- */

typedef struct {
    NkUiPixelRect stack[NK_UI_SDLR_CLIP_DEPTH];
    int depth;
    bool empty;
} NkUiSdlrClip;

static NkUiPixelRect nk_ui_sdlr_clip_current(const NkUiSdlrClip *clip) {
    return clip->stack[clip->depth - 1];
}

static void nk_ui_sdlr_apply_clip(NkUiSdlr *r, NkUiSdlrClip *clip) {
    NkUiPixelRect rect = nk_ui_sdlr_clip_current(clip);
    SDL_Rect sdl_rect;
    clip->empty = rect.x1 <= rect.x0 || rect.y1 <= rect.y0;
    if (clip->empty) {
        return;
    }
    sdl_rect.x = rect.x0;
    sdl_rect.y = rect.y0;
    sdl_rect.w = rect.x1 - rect.x0;
    sdl_rect.h = rect.y1 - rect.y0;
    SDL_SetRenderClipRect(r->renderer, &sdl_rect);
}

static SDL_FColor nk_ui_sdlr_color(uint32_t rgba_pm) {
    SDL_FColor color;
    color.r = (float)((rgba_pm >> 24) & 0xFFu) / 255.0f;
    color.g = (float)((rgba_pm >> 16) & 0xFFu) / 255.0f;
    color.b = (float)((rgba_pm >> 8) & 0xFFu) / 255.0f;
    color.a = (float)(rgba_pm & 0xFFu) / 255.0f;
    return color;
}

static void nk_ui_sdlr_flush_quads(NkUiSdlr *r, int page, int quads) {
    const NkUiSdlrQuadVertex *v = r->quad_vertices;
    if (quads <= 0 || page < 0 || page >= r->page_count) {
        return;
    }
    SDL_RenderGeometryRaw(r->renderer, r->pages[page].texture,
                          &v->x, (int)sizeof(*v), &v->color, (int)sizeof(*v),
                          &v->u, (int)sizeof(*v), quads * 4,
                          r->quad_indices, quads * 6, (int)sizeof(int));
}

static bool nk_ui_sdlr_reserve_quads(NkUiSdlr *r, size_t glyphs) {
    size_t i;
    if (glyphs <= r->quad_capacity) {
        return true;
    }
    {
        NkUiSdlrQuadVertex *vertices = (NkUiSdlrQuadVertex *)realloc(
            r->quad_vertices, glyphs * 4u * sizeof(*vertices));
        int *indices;
        if (vertices == NULL) {
            return false;
        }
        r->quad_vertices = vertices;
        indices = (int *)realloc(r->quad_indices, glyphs * 6u * sizeof(*indices));
        if (indices == NULL) {
            return false;
        }
        r->quad_indices = indices;
    }
    for (i = 0; i < glyphs; ++i) {
        int base = (int)(i * 4u);
        int *index = r->quad_indices + i * 6u;
        index[0] = base;
        index[1] = base + 1;
        index[2] = base + 2;
        index[3] = base;
        index[4] = base + 2;
        index[5] = base + 3;
    }
    r->quad_capacity = glyphs;
    return true;
}

static int nk_ui_sdlr_round(float value) {
    return (int)floorf(value + 0.5f);
}

static bool nk_ui_sdlr_draw_text(NkUiSdlr *r, const NkUiDrawCmd *cmd) {
    const NkUiTextRun *run = (const NkUiTextRun *)cmd->custom;
    NkUiTextSize size;
    SDL_FColor color;
    size_t needed;
    int count;
    int i;
    int origin_x;
    int baseline_y;
    int pending_page = -1;
    int pending_quads = 0;
    if (run == NULL || run->text == NULL || run->length == 0 || run->size_px <= 0 ||
        r->text.shape == NULL || r->text.raster == NULL) {
        return true;
    }
    needed = run->length < NK_UI_SDLR_MAX_GLYPHS_PER_RUN ? run->length : NK_UI_SDLR_MAX_GLYPHS_PER_RUN;
    if (needed > r->glyph_capacity) {
        NkUiGlyph *grown = (NkUiGlyph *)realloc(r->glyphs, needed * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        r->glyphs = grown;
        r->glyph_capacity = needed;
    }
    if (!nk_ui_sdlr_reserve_quads(r, needed)) {
        return false;
    }
    count = r->text.shape(r->text.user, run->font, run->size_px, run->text, run->length,
                          r->glyphs, (int)needed);
    if (count <= 0) {
        return true;
    }
    memset(&size, 0, sizeof(size));
    if (r->text.measure != NULL) {
        size = r->text.measure(r->text.user, run->font, run->size_px, run->text, run->length);
    }
    /* Centre the face's line box in the command's bounds (CSS half-leading),
       then snap: the baseline is a whole pixel row. */
    origin_x = cmd->bounds[0];
    if (run->center) {
        origin_x += nk_ui_sdlr_round(((float)(cmd->bounds[2] - cmd->bounds[0]) -
                                      size.width) * 0.5f);
    }
    baseline_y = cmd->bounds[1] +
                 nk_ui_sdlr_round(((float)(cmd->bounds[3] - cmd->bounds[1]) - size.height) * 0.5f) +
                 nk_ui_sdlr_round(size.baseline);
    color = nk_ui_sdlr_color(cmd->rgba_pm);
    for (i = 0; i < count; ++i) {
        const NkUiGlyph *glyph = &r->glyphs[i];
        NkUiSdlrEntry *entry = nk_ui_sdlr_glyph(r, run->font, run->size_px, glyph->glyph,
                                                &pending_page, &pending_quads);
        NkUiSdlrQuadVertex *q;
        float x0;
        float y0;
        float x1;
        float y1;
        float u0;
        float v0;
        float u1;
        float v1;
        float inverse;
        int k;
        if (entry == NULL) {
            nk_ui_sdlr_flush_quads(r, pending_page, pending_quads);
            return false;
        }
        if (entry->page < 0) {
            continue;
        }
        if (pending_quads > 0 && entry->page != pending_page) {
            nk_ui_sdlr_flush_quads(r, pending_page, pending_quads);
            pending_quads = 0;
        }
        pending_page = entry->page;
        x0 = (float)(origin_x + nk_ui_sdlr_round(glyph->x + run->letter_spacing * (float)i) +
                     entry->bearing_x);
        y0 = (float)(baseline_y + nk_ui_sdlr_round(glyph->y) - entry->bearing_y);
        x1 = x0 + (float)entry->w;
        y1 = y0 + (float)entry->h;
        inverse = 1.0f / (float)r->page_size;
        u0 = (float)entry->x * inverse;
        v0 = (float)entry->y * inverse;
        u1 = (float)(entry->x + entry->w) * inverse;
        v1 = (float)(entry->y + entry->h) * inverse;
        q = r->quad_vertices + (size_t)pending_quads * 4u;
        q[0].x = x0; q[0].y = y0; q[0].u = u0; q[0].v = v0;
        q[1].x = x1; q[1].y = y0; q[1].u = u1; q[1].v = v0;
        q[2].x = x1; q[2].y = y1; q[2].u = u1; q[2].v = v1;
        q[3].x = x0; q[3].y = y1; q[3].u = u0; q[3].v = v1;
        for (k = 0; k < 4; ++k) {
            q[k].color = color;
        }
        ++pending_quads;
        ++r->stats.glyphs_drawn;
    }
    nk_ui_sdlr_flush_quads(r, pending_page, pending_quads);
    ++r->stats.text_runs_drawn;
    return true;
}

/* ---- public ---- */

NkUiSdlr *nk_ui_sdlr_create(struct SDL_Renderer *renderer, const NkUiTextBackend *text) {
    NkUiSdlr *r;
    SDL_PropertiesID props;
    Sint64 max_texture;
    if (renderer == NULL) {
        return NULL;
    }
    r = (NkUiSdlr *)calloc(1, sizeof(*r));
    if (r == NULL) {
        return NULL;
    }
    r->renderer = renderer;
    if (text != NULL) {
        r->text = *text;
    }
    r->density = 0.0f;
    r->page_size = NK_UI_SDLR_PAGE_SIZE;
    props = SDL_GetRendererProperties(renderer);
    max_texture = props != 0 ? SDL_GetNumberProperty(props,
                                  SDL_PROP_RENDERER_MAX_TEXTURE_SIZE_NUMBER, 0) : 0;
    if (max_texture > 0 && max_texture < r->page_size) {
        r->page_size = (int)max_texture;
    }
    return r;
}

void nk_ui_sdlr_destroy(NkUiSdlr *r) {
    if (r == NULL) {
        return;
    }
    nk_ui_sdlr_drop_atlas(r);
    free(r->entries);
    free(r->table);
    free(r->colors);
    free(r->glyphs);
    free(r->quad_vertices);
    free(r->quad_indices);
    free(r->upload);
    if (r->waves_texture != NULL) {
        SDL_DestroyTexture(r->waves_texture);
    }
    for (size_t i = 0; i < NK_UI_SDLR_IMAGE_CACHE; ++i) {
        if (r->images[i].texture != NULL) {
            SDL_DestroyTexture(r->images[i].texture);
        }
    }
    free(r->waves_pixels);
    free(r);
}

void nk_ui_sdlr_set_scale(NkUiSdlr *r, float density) {
    if (r == NULL || !(density > 0.0f) || !isfinite(density) || density == r->density) {
        return;
    }
    if (r->page_count > 0 || r->entry_count > 0) {
        ++r->stats.atlas_rebuilds;
    }
    nk_ui_sdlr_drop_atlas(r);
    r->density = density;
}

void nk_ui_sdlr_get_stats(const NkUiSdlr *r, NkUiSdlrStats *out) {
    if (out == NULL) {
        return;
    }
    if (r == NULL) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = r->stats;
    out->atlas_pages = (uint32_t)r->page_count;
    out->atlas_glyphs = (uint32_t)r->entry_count;
}

/* The texture for an image, uploaded on first use. A full cache gives up the
   entry drawn longest ago; one drawn in this render is never evicted, so an
   image beyond that is uploaded for this draw only (*transient is set). */
static SDL_Texture *nk_ui_sdlr_image_texture(NkUiSdlr *r, const NkUiImage *image,
                                             int width, int height, bool *transient) {
    NkUiSdlrImageEntry *slot = NULL;
    SDL_Texture *texture;
    uint8_t *shrunk = NULL;
    const uint8_t *pixels = image->rgba_pm;
    size_t i;
    *transient = false;
    for (i = 0; i < NK_UI_SDLR_IMAGE_CACHE; ++i) {
        if (r->images[i].serial == image->serial && r->images[i].texture != NULL &&
            r->images[i].width == width && r->images[i].height == height) {
            r->images[i].last_used = r->render_count;
            return r->images[i].texture;
        }
    }
    for (i = 0; i < NK_UI_SDLR_IMAGE_CACHE; ++i) {
        NkUiSdlrImageEntry *entry = &r->images[i];
        if (entry->texture == NULL) {
            slot = entry;
            break;
        }
        if (entry->last_used != r->render_count &&
            (slot == NULL || entry->last_used < slot->last_used)) {
            slot = entry;
        }
    }
    if (width != (int)image->width || height != (int)image->height) {
        shrunk = (uint8_t *)malloc((size_t)width * (size_t)height * 4u);
        if (shrunk == NULL ||
            !nk_ui_image_resample(image, (uint32_t)width, (uint32_t)height, shrunk)) {
            free(shrunk);
            return NULL;
        }
        pixels = shrunk;
    }
    texture = SDL_CreateTexture(r->renderer, SDL_PIXELFORMAT_RGBA32,
                                SDL_TEXTUREACCESS_STATIC, width, height);
    if (texture == NULL) {
        free(shrunk);
        return NULL;
    }
    SDL_SetTextureBlendMode(texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    if (!SDL_UpdateTexture(texture, NULL, pixels, width * 4)) {
        SDL_DestroyTexture(texture);
        free(shrunk);
        return NULL;
    }
    free(shrunk);
    ++r->stats.image_uploads;
    if (slot == NULL) {
        *transient = true;
        return texture;
    }
    if (slot->texture != NULL) {
        SDL_DestroyTexture(slot->texture);
    }
    slot->serial = image->serial;
    slot->width = width;
    slot->height = height;
    slot->last_used = r->render_count;
    slot->texture = texture;
    return texture;
}

static bool nk_ui_sdlr_draw_image(NkUiSdlr *r, const NkUiDrawCmd *cmd) {
    const NkUiImage *image = (const NkUiImage *)cmd->custom;
    static const int indices[6] = {0, 1, 2, 0, 2, 3};
    NkUiSdlrQuadVertex q[4];
    SDL_Texture *texture;
    SDL_FColor color;
    bool transient = false;
    int width;
    int height;
    int k;
    if (image == NULL || image->rgba_pm == NULL || image->width == 0u || image->height == 0u ||
        image->width > (uint32_t)NK_UI_SDLR_IMAGE_MAX_SIDE ||
        image->height > (uint32_t)NK_UI_SDLR_IMAGE_MAX_SIDE ||
        cmd->bounds[2] <= cmd->bounds[0] || cmd->bounds[3] <= cmd->bounds[1]) {
        return true;
    }
    /* Shrunk by more than a quarter: upload an area-averaged copy at the drawn
       size, so a 64 px glyph drawn at 24 px does not alias. */
    width = (int)image->width;
    height = (int)image->height;
    if ((cmd->bounds[2] - cmd->bounds[0]) * 5 < width * 4 ||
        (cmd->bounds[3] - cmd->bounds[1]) * 5 < height * 4) {
        width = cmd->bounds[2] - cmd->bounds[0];
        height = cmd->bounds[3] - cmd->bounds[1];
    }
    texture = nk_ui_sdlr_image_texture(r, image, width, height, &transient);
    if (texture == NULL) {
        ++r->stats.images_failed;
        return true; /* an image that cannot upload is left out, never fatal */
    }
    color = nk_ui_sdlr_color(cmd->rgba_pm);
    q[0].x = (float)cmd->bounds[0]; q[0].y = (float)cmd->bounds[1]; q[0].u = 0.0f; q[0].v = 0.0f;
    q[1].x = (float)cmd->bounds[2]; q[1].y = (float)cmd->bounds[1]; q[1].u = 1.0f; q[1].v = 0.0f;
    q[2].x = (float)cmd->bounds[2]; q[2].y = (float)cmd->bounds[3]; q[2].u = 1.0f; q[2].v = 1.0f;
    q[3].x = (float)cmd->bounds[0]; q[3].y = (float)cmd->bounds[3]; q[3].u = 0.0f; q[3].v = 1.0f;
    for (k = 0; k < 4; ++k) {
        q[k].color = color;
    }
    SDL_RenderGeometryRaw(r->renderer, texture, &q[0].x, (int)sizeof(q[0]),
                          &q[0].color, (int)sizeof(q[0]), &q[0].u, (int)sizeof(q[0]),
                          4, indices, 6, (int)sizeof(int));
    if (transient) {
        SDL_DestroyTexture(texture);
    }
    ++r->stats.images_drawn;
    return true;
}

/* Draws one waves command: the frame is re-rendered only when its phases, size
   or palette changed, so a held-still frame costs one textured quad. */
static bool nk_ui_sdlr_draw_waves(NkUiSdlr *r, const NkUiDrawCmd *cmd) {
    const NkUiWavesCommand *command = (const NkUiWavesCommand *)cmd->custom;
    int bw = cmd->bounds[2] - cmd->bounds[0];
    int bh = cmd->bounds[3] - cmd->bounds[1];
    int w;
    int h;
    SDL_FRect dst;
    if (command == NULL || bw <= 0 || bh <= 0) {
        return true;
    }
    w = (bw + NK_UI_SDLR_WAVES_DIVISOR - 1) / NK_UI_SDLR_WAVES_DIVISOR;
    if (w > NK_UI_SDLR_WAVES_MAX_W) {
        w = NK_UI_SDLR_WAVES_MAX_W;
    }
    h = (int)((long long)bh * w / bw);
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (r->waves_texture == NULL || r->waves_w != w || r->waves_h != h) {
        uint8_t *pixels;
        if (r->waves_texture != NULL) {
            SDL_DestroyTexture(r->waves_texture);
            r->waves_texture = NULL;
        }
        pixels = (uint8_t *)realloc(r->waves_pixels, (size_t)w * (size_t)h * 4u);
        if (pixels == NULL) {
            return false;
        }
        r->waves_pixels = pixels;
        r->waves_texture = SDL_CreateTexture(r->renderer, SDL_PIXELFORMAT_RGBA32,
                                             SDL_TEXTUREACCESS_STATIC, w, h);
        if (r->waves_texture == NULL) {
            return false;
        }
        SDL_SetTextureBlendMode(r->waves_texture, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(r->waves_texture, SDL_SCALEMODE_LINEAR);
        r->waves_w = w;
        r->waves_h = h;
        r->waves_valid = false;
    }
    if (!r->waves_valid || memcmp(&r->waves_last, command, sizeof(*command)) != 0) {
        if (!nk_ui_waves_render_command(command, (uint32_t)w, (uint32_t)h, r->waves_pixels,
                                        (size_t)w * 4u) ||
            !SDL_UpdateTexture(r->waves_texture, NULL, r->waves_pixels, w * 4)) {
            return false;
        }
        r->waves_last = *command;
        r->waves_valid = true;
        ++r->stats.waves_rendered;
    }
    dst.x = (float)cmd->bounds[0];
    dst.y = (float)cmd->bounds[1];
    dst.w = (float)bw;
    dst.h = (float)bh;
    ++r->stats.waves_drawn;
    return SDL_RenderTexture(r->renderer, r->waves_texture, NULL, &dst);
}

static bool nk_ui_sdlr_list_valid(const NkUiDrawList *list) {
    uint32_t i;
    if (list->nv > (uint32_t)INT_MAX || list->ni > (uint32_t)INT_MAX ||
        (list->nv > 0 && list->v == NULL) || (list->ni > 0 && list->idx == NULL) ||
        (list->ncmd > 0 && list->cmd == NULL)) {
        return false;
    }
    for (i = 0; i < list->ni; ++i) {
        if (list->idx[i] >= list->nv) {
            return false;
        }
    }
    for (i = 0; i < list->ncmd; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        if (cmd->kind == NK_UI_DRAW_RECTANGLE || cmd->kind == NK_UI_DRAW_BORDER) {
            if (cmd->first > list->ni || cmd->count > list->ni - cmd->first) {
                return false;
            }
        }
    }
    return true;
}

static void nk_ui_sdlr_draw_triangles(NkUiSdlr *r, const NkUiDrawList *list,
                                      uint32_t first, uint32_t count) {
    if (count == 0) {
        return;
    }
    SDL_RenderGeometryRaw(r->renderer, NULL,
                          &list->v[0].x, (int)sizeof(NkUiVertex),
                          r->colors, (int)sizeof(SDL_FColor),
                          &list->v[0].u, (int)sizeof(NkUiVertex),
                          (int)list->nv, list->idx + first, (int)count, (int)sizeof(uint32_t));
}

bool nk_ui_sdlr_render(NkUiSdlr *r, const NkUiDrawList *list) {
    NkUiSdlrClip clip;
    SDL_Rect previous_clip;
    SDL_BlendMode previous_blend = SDL_BLENDMODE_NONE;
    bool had_clip;
    bool ok = true;
    uint32_t batch_first = 0;
    uint32_t batch_count = 0;
    uint32_t i;
    if (r == NULL || list == NULL || !nk_ui_sdlr_list_valid(list)) {
        return false;
    }
    ++r->render_count;
    if (list->nv > r->color_capacity) {
        SDL_FColor *grown = (SDL_FColor *)realloc(r->colors, list->nv * sizeof(*grown));
        if (grown == NULL) {
            return false;
        }
        r->colors = grown;
        r->color_capacity = list->nv;
    }
    for (i = 0; i < list->nv; ++i) {
        r->colors[i] = nk_ui_sdlr_color(list->v[i].rgba_pm);
    }
    had_clip = SDL_RenderClipEnabled(r->renderer);
    memset(&previous_clip, 0, sizeof(previous_clip));
    if (had_clip) {
        SDL_GetRenderClipRect(r->renderer, &previous_clip);
    }
    SDL_GetRenderDrawBlendMode(r->renderer, &previous_blend);
    SDL_SetRenderDrawBlendMode(r->renderer, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    memset(&clip, 0, sizeof(clip));
    clip.depth = 1;
    clip.stack[0].x0 = INT_MIN / 2;
    clip.stack[0].y0 = INT_MIN / 2;
    clip.stack[0].x1 = INT_MAX / 2;
    clip.stack[0].y1 = INT_MAX / 2;
    SDL_SetRenderClipRect(r->renderer, NULL);

    for (i = 0; i < list->ncmd && ok; ++i) {
        const NkUiDrawCmd *cmd = &list->cmd[i];
        bool geometry = cmd->kind == NK_UI_DRAW_RECTANGLE || cmd->kind == NK_UI_DRAW_BORDER;
        if (geometry && !clip.empty) {
            /* Commands append their indices in order, so neighbours batch. */
            if (batch_count > 0 && cmd->first == batch_first + batch_count) {
                batch_count += cmd->count;
            } else {
                nk_ui_sdlr_draw_triangles(r, list, batch_first, batch_count);
                batch_first = cmd->first;
                batch_count = cmd->count;
            }
            continue;
        }
        nk_ui_sdlr_draw_triangles(r, list, batch_first, batch_count);
        batch_count = 0;
        switch (cmd->kind) {
        case NK_UI_DRAW_RECTANGLE:
        case NK_UI_DRAW_BORDER:
            break; /* clipped away */
        case NK_UI_DRAW_TEXT:
            if (!clip.empty) {
                ok = nk_ui_sdlr_draw_text(r, cmd);
            }
            break;
        case NK_UI_DRAW_SCISSOR_START:
            if (clip.depth < NK_UI_SDLR_CLIP_DEPTH) {
                NkUiPixelRect parent = nk_ui_sdlr_clip_current(&clip);
                NkUiPixelRect next;
                next.x0 = cmd->clip[0] > parent.x0 ? cmd->clip[0] : parent.x0;
                next.y0 = cmd->clip[1] > parent.y0 ? cmd->clip[1] : parent.y0;
                next.x1 = cmd->clip[2] < parent.x1 ? cmd->clip[2] : parent.x1;
                next.y1 = cmd->clip[3] < parent.y1 ? cmd->clip[3] : parent.y1;
                clip.stack[clip.depth++] = next;
                nk_ui_sdlr_apply_clip(r, &clip);
            } else {
                ok = false; /* nesting deeper than any real screen */
            }
            break;
        case NK_UI_DRAW_SCISSOR_END:
            if (clip.depth > 1) {
                --clip.depth;
                if (clip.depth == 1) {
                    SDL_SetRenderClipRect(r->renderer, NULL);
                    clip.empty = false;
                } else {
                    nk_ui_sdlr_apply_clip(r, &clip);
                }
            }
            break;
        case NK_UI_DRAW_CUSTOM:
            if (cmd->custom_kind == NK_UI_CUSTOM_WAVES) {
                if (!clip.empty) {
                    ok = nk_ui_sdlr_draw_waves(r, cmd);
                }
            } else if (cmd->custom_kind == NK_UI_CUSTOM_IMAGE) {
                if (!clip.empty) {
                    ok = nk_ui_sdlr_draw_image(r, cmd);
                }
            } else {
                ++r->stats.custom_skipped;
            }
            break;
        default:
            ++r->stats.custom_skipped;
            break;
        }
    }
    nk_ui_sdlr_draw_triangles(r, list, batch_first, batch_count);

    SDL_SetRenderDrawBlendMode(r->renderer, previous_blend);
    SDL_SetRenderClipRect(r->renderer, had_clip ? &previous_clip : NULL);
    return ok;
}

#endif /* NK_UI_WITH_SDL */
