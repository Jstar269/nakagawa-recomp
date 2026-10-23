/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Prompt glyphs for the four neutral prompt styles, drawn as nk_ui geometry.
   No font, image or console-maker symbol is involved: dots, circles, keycaps and a
   D-pad cross are rounded rectangles, and a letter or key name is ordinary text
   drawn through the draw list. */

#include "nk_ui_input.h"

#include <math.h>
#include <string.h>

/* Prompt text per style, indexed by action up to MENU. NULL means pure geometry. */
static const char *const g_letters_a_south[NK_UI_PROMPT_ACT_MENU + 1] = {
    "A", "B", "X", "Y", "L", "R", NULL
};
static const char *const g_letters_a_east[NK_UI_PROMPT_ACT_MENU + 1] = {
    "B", "A", "Y", "X", "L", "R", NULL
};
static const char *const g_neutral[NK_UI_PROMPT_ACT_MENU + 1] = {
    NULL, NULL, NULL, NULL, "L", "R", NULL
};
static const char *const g_keycaps[NK_UI_PROMPT_ACT_MENU + 1] = {
    "ENTER", "ESC", "SPACE", "D", "Q", "E", "F1"
};

NkUiPadFamily nk_ui_pad_family_for_south_label(NkUiButtonLabel label) {
    switch (label) {
    case NK_UI_LABEL_A: return NK_UI_PAD_LETTERS_A_SOUTH;
    case NK_UI_LABEL_B: return NK_UI_PAD_LETTERS_A_EAST;
    case NK_UI_LABEL_UNKNOWN:
    case NK_UI_LABEL_X:
    case NK_UI_LABEL_Y:
    case NK_UI_LABEL_CROSS:
    case NK_UI_LABEL_CIRCLE:
    case NK_UI_LABEL_SQUARE:
    case NK_UI_LABEL_TRIANGLE:
    case NK_UI_LABEL_COUNT:
    default: return NK_UI_PAD_NEUTRAL;
    }
}

NkUiPromptStyle nk_ui_prompt_style_for_family(NkUiPadFamily family) {
    switch (family) {
    case NK_UI_PAD_LETTERS_A_SOUTH: return NK_UI_PROMPT_LETTERS_A_SOUTH;
    case NK_UI_PAD_LETTERS_A_EAST: return NK_UI_PROMPT_LETTERS_A_EAST;
    case NK_UI_PAD_KEYBOARD: return NK_UI_PROMPT_KEYCAPS;
    case NK_UI_PAD_NEUTRAL:
    default: return NK_UI_PROMPT_NEUTRAL;
    }
}

const char *nk_ui_prompt_text(NkUiPromptStyle style, NkUiPromptAction action) {
    if ((unsigned)action > (unsigned)NK_UI_PROMPT_ACT_MENU) {
        return NULL;
    }
    switch (style) {
    case NK_UI_PROMPT_LETTERS_A_SOUTH: return g_letters_a_south[action];
    case NK_UI_PROMPT_LETTERS_A_EAST: return g_letters_a_east[action];
    case NK_UI_PROMPT_KEYCAPS: return g_keycaps[action];
    case NK_UI_PROMPT_NEUTRAL:
    default: return g_neutral[action];
    }
}

static bool nk_ui_prompt_is_shoulder(NkUiPromptAction action) {
    return action == NK_UI_PROMPT_ACT_TAB_PREV || action == NK_UI_PROMPT_ACT_TAB_NEXT;
}

float nk_ui_prompt_width(NkUiPromptStyle style, NkUiPromptAction action, float height) {
    const char *text;
    if (!isfinite(height) || height <= 0.0f) {
        return 0.0f;
    }
    if (action == NK_UI_PROMPT_ACT_MOVE) {
        /* The arrow keys sit as an inverted T, three keys wide. */
        return style == NK_UI_PROMPT_KEYCAPS ? height * 1.5f : height;
    }
    if (style == NK_UI_PROMPT_KEYCAPS && action != NK_UI_PROMPT_ACT_MOVE) {
        size_t n;
        text = nk_ui_prompt_text(style, action);
        n = text != NULL ? strlen(text) : 1u;
        return n <= 1u ? height : height * (0.5f + 0.28f * (float)n);
    }
    if (nk_ui_prompt_is_shoulder(action)) {
        return height * 1.6f;
    }
    return height;
}

/* Glyphs are laid out in device pixels with integer sizes and offsets, then handed to
   the draw list as logical rects that round back to exactly those pixels. That keeps a
   dot round, a cross symmetric and a keycap edge a whole 3 px at every scale, instead of
   letting fractional layout round to uneven shapes. */
typedef struct {
    NkUiDrawBuffer *buffer;
    const NkUiPromptSpec *spec;
    float density;
    uint32_t id;
    int x0;
    int y0;
    int w;
    int h;
    int side;     /* even square inside the box */
    int cx;       /* integer center of that square */
    int cy;
} NkUiPromptPx;

static bool nk_ui_prompt_rect(const NkUiPromptPx *p, int x0, int y0, int x1, int y1,
                              int radius_px, uint32_t rgba) {
    NkUiRect r;
    r.x = (float)x0 / p->density;
    r.y = (float)y0 / p->density;
    r.w = (float)(x1 - x0) / p->density;
    r.h = (float)(y1 - y0) / p->density;
    return nk_ui_draw_add_rect(p->buffer, r, p->density, (float)radius_px / p->density,
                               rgba, p->id);
}

static bool nk_ui_prompt_text_in(const NkUiPromptPx *p, const char *text, int x0, int y0,
                                 int x1, int y1, int size_px) {
    NkUiPixelRect bounds;
    if (text == NULL) {
        return true;
    }
    bounds.x0 = x0;
    bounds.y0 = y0;
    bounds.x1 = x1;
    bounds.y1 = y1;
    if (size_px < 1) {
        size_px = 1;
    }
    return nk_ui_draw_add_text_centered(p->buffer, bounds, text, strlen(text), p->spec->font,
                                        size_px, p->spec->fg_rgba, p->id);
}

static int nk_ui_prompt_even(int v) {
    return v < 2 ? 2 : (v & ~1);
}

static bool nk_ui_prompt_dots(const NkUiPromptPx *p) {
    /* Four dots in the button layout, the action's position filled. */
    int r = p->side * 15 / 100;
    int d = p->side * 30 / 100;
    static const int dx[4] = {0, 1, 0, -1};   /* north, east, south, west */
    static const int dy[4] = {-1, 0, 1, 0};
    int active;
    int i;
    int plate = nk_ui_prompt_even(p->side);
    if (r < 1) {
        r = 1;
    }
    /* The dots sit on a round plate with an edge, one button cluster. */
    if (!nk_ui_prompt_rect(p, p->cx - plate / 2, p->cy - plate / 2, p->cx + plate / 2,
                           p->cy + plate / 2, plate / 2, p->spec->dim_rgba) ||
        !nk_ui_prompt_rect(p, p->cx - plate / 2 + 1, p->cy - plate / 2 + 1,
                           p->cx + plate / 2 - 1, p->cy + plate / 2 - 1, plate / 2 - 1,
                           p->spec->face_rgba)) {
        return false;
    }
    switch (p->spec->action) {
    case NK_UI_PROMPT_ACT_CONFIRM: active = 2; break;   /* south */
    case NK_UI_PROMPT_ACT_BACK: active = 1; break;      /* east */
    case NK_UI_PROMPT_ACT_OPTIONS: active = 3; break;   /* west */
    default: active = 0; break;                          /* details: north */
    }
    for (i = 0; i < 4; ++i) {
        int x = p->cx + dx[i] * d;
        int y = p->cy + dy[i] * d;
        if (!nk_ui_prompt_rect(p, x - r, y - r, x + r, y + r, r,
                               i == active ? p->spec->fg_rgba : p->spec->dim_rgba)) {
            return false;
        }
    }
    return true;
}

static bool nk_ui_prompt_circle_letter(const NkUiPromptPx *p) {
    int dia = nk_ui_prompt_even(p->side * 9 / 10);
    int x0 = p->cx - dia / 2;
    int y0 = p->cy - dia / 2;
    return nk_ui_prompt_rect(p, x0, y0, x0 + dia, y0 + dia, dia / 2, p->spec->face_rgba) &&
           nk_ui_prompt_text_in(p, nk_ui_prompt_text(p->spec->style, p->spec->action),
                                x0, y0, x0 + dia, y0 + dia, dia * 55 / 100);
}

static bool nk_ui_prompt_keycap(const NkUiPromptPx *p) {
    /* A rounded face over a rounded base that shows as a 3 px bottom border. */
    int radius = p->h * 20 / 100;
    int edge = (int)floorf(3.0f * p->density + 0.5f);
    int face_h;
    if (edge > p->h * 40 / 100) {
        edge = p->h * 40 / 100;
    }
    if (edge < 1) {
        edge = 1;
    }
    face_h = p->h - edge;
    return nk_ui_prompt_rect(p, p->x0, p->y0, p->x0 + p->w, p->y0 + p->h, radius,
                             p->spec->dim_rgba) &&
           nk_ui_prompt_rect(p, p->x0, p->y0, p->x0 + p->w, p->y0 + face_h, radius,
                             p->spec->face_rgba) &&
           nk_ui_prompt_text_in(p, nk_ui_prompt_text(p->spec->style, p->spec->action),
                                p->x0, p->y0, p->x0 + p->w, p->y0 + face_h, face_h * 50 / 100);
}

static bool nk_ui_prompt_pill(const NkUiPromptPx *p) {
    return nk_ui_prompt_rect(p, p->x0, p->y0, p->x0 + p->w, p->y0 + p->h, p->h / 2,
                             p->spec->face_rgba) &&
           nk_ui_prompt_text_in(p, nk_ui_prompt_text(p->spec->style, p->spec->action),
                                p->x0, p->y0, p->x0 + p->w, p->y0 + p->h, p->h * 50 / 100);
}

static bool nk_ui_prompt_menu(const NkUiPromptPx *p) {
    int plate = nk_ui_prompt_even(p->side * 9 / 10);
    int bar_w = nk_ui_prompt_even(p->side / 2);
    int bar_h = nk_ui_prompt_even(p->side * 8 / 100);
    int gap = p->side * 20 / 100;
    int i;
    if (!nk_ui_prompt_rect(p, p->cx - plate / 2, p->cy - plate / 2, p->cx + plate / 2,
                           p->cy + plate / 2, plate / 5, p->spec->face_rgba)) {
        return false;
    }
    for (i = -1; i <= 1; ++i) {
        int y = p->cy + i * gap;
        if (!nk_ui_prompt_rect(p, p->cx - bar_w / 2, y - bar_h / 2, p->cx + bar_w / 2,
                               y + bar_h / 2, bar_h / 2, p->spec->fg_rgba)) {
            return false;
        }
    }
    return true;
}

/* A small filled triangle pointing up, right, down or left (0..3), centred on
   (cx, cy) with half-size r, in device pixels. */
static bool nk_ui_prompt_arrow(const NkUiPromptPx *p, float cx, float cy, float r, int dir) {
    float xy[6];
    switch (dir) {
    case 0: xy[0] = cx; xy[1] = cy - r; xy[2] = cx + r; xy[3] = cy + r * 0.6f;
        xy[4] = cx - r; xy[5] = cy + r * 0.6f; break;
    case 1: xy[0] = cx + r; xy[1] = cy; xy[2] = cx - r * 0.6f; xy[3] = cy + r;
        xy[4] = cx - r * 0.6f; xy[5] = cy - r; break;
    case 2: xy[0] = cx; xy[1] = cy + r; xy[2] = cx - r; xy[3] = cy - r * 0.6f;
        xy[4] = cx + r; xy[5] = cy - r * 0.6f; break;
    default: xy[0] = cx - r; xy[1] = cy; xy[2] = cx + r * 0.6f; xy[3] = cy - r;
        xy[4] = cx + r * 0.6f; xy[5] = cy + r; break;
    }
    return nk_ui_draw_add_convex(p->buffer, xy, 3u, p->spec->fg_rgba, p->id);
}

/* MOVE on a controller: a D-pad, a raised cross with an edge and an arrow at
   the end of each arm, so it reads as the pad and never as a key. */
static bool nk_ui_prompt_dpad(const NkUiPromptPx *p) {
    int arm = nk_ui_prompt_even(p->side);
    int thick = nk_ui_prompt_even(p->side * 38 / 100);
    int edge = (int)floorf(p->density + 0.5f);
    float reach = (float)arm * 0.5f - (float)thick * 0.42f;
    float r = (float)thick * 0.3f;
    int dir;
    if (edge < 1) edge = 1;
    if (!nk_ui_prompt_rect(p, p->cx - arm / 2, p->cy - thick / 2, p->cx + arm / 2,
                           p->cy + thick / 2, thick / 4, p->spec->dim_rgba) ||
        !nk_ui_prompt_rect(p, p->cx - thick / 2, p->cy - arm / 2, p->cx + thick / 2,
                           p->cy + arm / 2, thick / 4, p->spec->dim_rgba) ||
        !nk_ui_prompt_rect(p, p->cx - arm / 2 + edge, p->cy - thick / 2 + edge,
                           p->cx + arm / 2 - edge, p->cy + thick / 2 - edge, thick / 4,
                           p->spec->face_rgba) ||
        !nk_ui_prompt_rect(p, p->cx - thick / 2 + edge, p->cy - arm / 2 + edge,
                           p->cx + thick / 2 - edge, p->cy + arm / 2 - edge, thick / 4,
                           p->spec->face_rgba)) {
        return false;
    }
    for (dir = 0; dir < 4; ++dir) {
        float dx = dir == 1 ? reach : dir == 3 ? -reach : 0.0f;
        float dy = dir == 2 ? reach : dir == 0 ? -reach : 0.0f;
        if (!nk_ui_prompt_arrow(p, (float)p->cx + dx, (float)p->cy + dy, r, dir)) {
            return false;
        }
    }
    return true;
}

/* MOVE on a keyboard: the four arrow keys as an inverted T of small keycaps,
   each with its arrow. */
static bool nk_ui_prompt_arrow_keys(const NkUiPromptPx *p) {
    int gap = p->h / 12 > 1 ? p->h / 12 : 1;
    int key = (p->h - gap) / 2 < (p->w - gap * 2) / 3 ? (p->h - gap) / 2 : (p->w - gap * 2) / 3;
    int edge = key / 6 > 1 ? key / 6 : 1;
    int total = key * 3 + gap * 2;
    int x0 = p->x0 + (p->w - total) / 2;
    int y0 = p->y0 + (p->h - (key * 2 + gap)) / 2;
    int radius = key / 4;
    int k;
    /* up, then left, down, right */
    const int kx[4] = {x0 + key + gap, x0, x0 + key + gap, x0 + (key + gap) * 2};
    const int ky[4] = {y0, y0 + key + gap, y0 + key + gap, y0 + key + gap};
    const int dirs[4] = {0, 3, 2, 1};
    if (key < 2) {
        return true;
    }
    for (k = 0; k < 4; ++k) {
        float cx = (float)kx[k] + (float)key * 0.5f;
        float cy = (float)ky[k] + (float)(key - edge) * 0.5f;
        if (!nk_ui_prompt_rect(p, kx[k], ky[k], kx[k] + key, ky[k] + key, radius,
                               p->spec->dim_rgba) ||
            !nk_ui_prompt_rect(p, kx[k], ky[k], kx[k] + key, ky[k] + key - edge, radius,
                               p->spec->face_rgba) ||
            !nk_ui_prompt_arrow(p, cx, cy, (float)key * 0.2f, dirs[k])) {
            return false;
        }
    }
    return true;
}

bool nk_ui_prompt_draw(NkUiDrawBuffer *buffer, const NkUiPromptSpec *spec,
                       NkUiRect rect, float density, uint32_t element_id) {
    NkUiPromptPx p;
    NkUiPixelRect box;
    NkUiPromptStyle style;
    int shorter;
    if (buffer == NULL || spec == NULL || !isfinite(density) || density <= 0.0f ||
        !isfinite(rect.x) || !isfinite(rect.y) || !isfinite(rect.w) || !isfinite(rect.h) ||
        rect.w <= 0.0f || rect.h <= 0.0f ||
        (unsigned)spec->action >= (unsigned)NK_UI_PROMPT_ACT_COUNT) {
        return false;
    }
    box = nk_ui_round_rect(rect, density);
    if (box.x1 <= box.x0 || box.y1 <= box.y0) {
        return true; /* rounds to nothing: nothing to draw */
    }
    memset(&p, 0, sizeof(p));
    p.buffer = buffer;
    p.spec = spec;
    p.density = density;
    p.id = element_id;
    p.x0 = box.x0;
    p.y0 = box.y0;
    p.w = box.x1 - box.x0;
    p.h = box.y1 - box.y0;
    shorter = p.w < p.h ? p.w : p.h;
    p.side = shorter & ~1;
    if (p.side < 2) {
        return true; /* too small to read */
    }
    /* Center the even square on whole pixels. */
    p.cx = p.x0 + (p.w - p.side) / 2 + p.side / 2;
    p.cy = p.y0 + (p.h - p.side) / 2 + p.side / 2;
    style = spec->style;
    switch (spec->action) {
    case NK_UI_PROMPT_ACT_MOVE:
        return style == NK_UI_PROMPT_KEYCAPS ? nk_ui_prompt_arrow_keys(&p) : nk_ui_prompt_dpad(&p);
    case NK_UI_PROMPT_ACT_MENU:
        return style == NK_UI_PROMPT_KEYCAPS ? nk_ui_prompt_keycap(&p) : nk_ui_prompt_menu(&p);
    case NK_UI_PROMPT_ACT_TAB_PREV:
    case NK_UI_PROMPT_ACT_TAB_NEXT:
        return style == NK_UI_PROMPT_KEYCAPS ? nk_ui_prompt_keycap(&p) : nk_ui_prompt_pill(&p);
    default:
        break;
    }
    switch (style) {
    case NK_UI_PROMPT_LETTERS_A_SOUTH:
    case NK_UI_PROMPT_LETTERS_A_EAST:
        return nk_ui_prompt_circle_letter(&p);
    case NK_UI_PROMPT_KEYCAPS:
        return nk_ui_prompt_keycap(&p);
    case NK_UI_PROMPT_NEUTRAL:
    default:
        return nk_ui_prompt_dots(&p);
    }
}
