/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * osk_overlay_paint.c - the in-window keyboard's drawing (contract in osk_overlay_paint.h).
 *
 * Layout on the 480x272 guest frame:
 *   y 6      the guest's description (ASCII; other characters show as '?')
 *   y 24-48  the text field, showing the end of the text when it is longer than the field
 *   y 56..   the key grid: seven rows of 26 units, ten 44-unit columns, the wide row at the bottom
 *   y 258    the controls
 * Labels use SDL's own 8x8 debug font, which is ASCII only.
 */
#include "osk_overlay_paint.h"

#include <SDL3/SDL_pixels.h>
#include <SDL3/SDL_render.h>
#include <SDL3/SDL_surface.h>
#include <stdio.h>
#include <string.h>

#define GLYPH       8          /* SDL_RenderDebugText character cell, pixels */
#define UNIT_W      44         /* one key column in the grid */
#define GRID_X      20
#define GRID_Y      56
#define ROW_H       28         /* key height plus gap */
#define KEY_H       24
#define KEY_GAP     2

static int s_probe = -1;       /* -1 unprobed, 0 unavailable, 1 available */

/* Returns 1 when a software renderer can be made on a surface of these pixels. */
static int paint_probe_once(void) {
    uint32_t px[4] = { 0, 0, 0, 0 };
    SDL_Surface *surface = SDL_CreateSurfaceFrom(2, 2, SDL_PIXELFORMAT_XRGB8888, px, 2 * 4);
    if (!surface) return 0;
    SDL_Renderer *renderer = SDL_CreateSoftwareRenderer(surface);
    int ok = renderer != NULL;
    if (renderer) SDL_DestroyRenderer(renderer);
    SDL_DestroySurface(surface);
    return ok;
}

int osk_overlay_paint_available(void) {
    if (s_probe < 0) s_probe = paint_probe_once();
    return s_probe;
}

static void fill(SDL_Renderer *r, float x, float y, float w, float h, Uint8 R, Uint8 G, Uint8 B,
                 Uint8 A) {
    SDL_FRect rect = { x, y, w, h };
    SDL_SetRenderDrawColor(r, R, G, B, A);
    SDL_RenderFillRect(r, &rect);
}

static void outline(SDL_Renderer *r, float x, float y, float w, float h, Uint8 R, Uint8 G,
                    Uint8 B) {
    SDL_FRect rect = { x, y, w, h };
    SDL_SetRenderDrawColor(r, R, G, B, 255);
    SDL_RenderRect(r, &rect);
}

/* ASCII for the debug font: anything else is a question mark. */
static char ascii_of(uint16_t u) {
    return (u >= 0x20u && u < 0x7Fu) ? (char)u : '?';
}

/* Draw up to `max` characters of text (UTF-16) starting at `first`, at (x, y). */
static void text_units(SDL_Renderer *r, float x, float y, const uint16_t *units, int first,
                       int count) {
    char line[OSK_OVERLAY_TEXT_MAX + 1];
    int n = 0;
    for (int i = first; i < first + count; i++) line[n++] = ascii_of(units[i]);
    line[n] = 0;
    SDL_RenderDebugText(r, x, y, line);
}

static void label_cell(SDL_Renderer *r, const OskCell *c, float x, float y, float w, float h) {
    char label[8];
    switch (c->kind) {
    case OSK_CELL_CHAR:  label[0] = c->ch; label[1] = 0; break;
    case OSK_CELL_SPACE: strcpy(label, "SPACE"); break;
    case OSK_CELL_BKSP:  strcpy(label, "BKSP"); break;
    case OSK_CELL_CASE:  strcpy(label, "CASE"); break;
    case OSK_CELL_CANCEL: strcpy(label, "CANCEL"); break;
    case OSK_CELL_OK:    strcpy(label, "OK"); break;
    default:             label[0] = 0; break;
    }
    float tw = (float)(strlen(label) * GLYPH);
    SDL_RenderDebugText(r, x + (w - tw) / 2.0f, y + (h - GLYPH) / 2.0f, label);
}

void osk_overlay_paint(const OskOverlay *o, uint32_t *px, int w, int h) {
    if (!o || !px || o->status != OSK_OVERLAY_OPEN) return;
    if (!osk_overlay_paint_available()) return;
    SDL_Surface *surface = SDL_CreateSurfaceFrom(w, h, SDL_PIXELFORMAT_XRGB8888, px, w * 4);
    if (!surface) return;
    SDL_Renderer *r = SDL_CreateSoftwareRenderer(surface);
    if (!r) {
        SDL_DestroySurface(surface);
        return;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);

    /* Panel: the game stays faintly visible behind the keyboard. */
    fill(r, 0, 0, (float)w, (float)h, 8, 10, 16, 206);

    /* Description. SDL's debug text takes the current draw colour, so each run sets its own. */
    int desc_len = 0;
    while (desc_len < OSK_OVERLAY_DESC_MAX - 1 && o->desc[desc_len]) desc_len++;
    int desc_max = (w - 2 * 12) / GLYPH;
    SDL_SetRenderDrawColor(r, 210, 220, 235, 255);
    text_units(r, 12, 6, o->desc, 0, desc_len < desc_max ? desc_len : desc_max);

    /* Text field: the end of the text is what the person is typing. */
    fill(r, 12, 24, (float)(w - 24), 24, 30, 36, 50, 255);
    outline(r, 12, 24, (float)(w - 24), 24, 90, 120, 180);
    int field_max = (w - 24 - 16) / GLYPH - 1;   /* one column left for the caret */
    int first = o->len > field_max ? o->len - field_max : 0;
    SDL_SetRenderDrawColor(r, 240, 244, 250, 255);
    text_units(r, 20, 32, o->text, first, o->len - first);
    SDL_RenderDebugText(r, 20 + (float)((o->len - first) * GLYPH), 32, "_");

    /* Key grid. */
    OskCell cells[OSK_OVERLAY_CELLS];
    int n = osk_overlay_cells(o, cells, OSK_OVERLAY_CELLS);
    for (int i = 0; i < n; i++) {
        const OskCell *c = &cells[i];
        float x = (float)(GRID_X + c->start * UNIT_W + KEY_GAP);
        float y = (float)(GRID_Y + c->row * ROW_H);
        float kw = (float)(c->span * UNIT_W - 2 * KEY_GAP);
        if (c->highlight) {
            fill(r, x, y, kw, KEY_H, 70, 120, 200, 255);
            outline(r, x, y, kw, KEY_H, 200, 220, 255);
        } else if (c->enabled) {
            fill(r, x, y, kw, KEY_H, 44, 56, 80, 255);
        } else {
            fill(r, x, y, kw, KEY_H, 24, 28, 38, 255);
        }
        SDL_SetRenderDrawColor(r, c->enabled ? 240 : 90, c->enabled ? 240 : 96,
                               c->enabled ? 245 : 110, 255);
        label_cell(r, c, x, y, kw, KEY_H);
    }

    /* Controls. */
    SDL_SetRenderDrawColor(r, 170, 182, 200, 255);
    SDL_RenderDebugText(r, 12, (float)(h - 14),
                        "Cross type  Circle cancel  Start OK  L/R case");

    SDL_RenderPresent(r);
    SDL_DestroyRenderer(r);
    SDL_DestroySurface(surface);
}
