/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

#ifndef NK_UI_THEME_H
#define NK_UI_THEME_H

/* The player's design system: colour tokens (one struct per theme), and the
   spacing, radius, type, layout and motion scales every screen draws with.
   docs/PLAYER_UI_DESIGN.md is the written spec; change both together. Sizes are
   logical px: the core multiplies by the UI scale and rounds rects, borders and
   rings to whole device pixels. */

#include "nk_ui.h"

/* Colors use 0xRRGGBBAA sRGB bytes. The first fourteen fields are the original
   C3 set; the rest were added with the C5 design pass. */
struct NkUiTheme {
    uint32_t ink;            /* #10151C text and icons on an accent fill */
    uint32_t paper;          /* #F5F7FA primary text */
    uint32_t surface;        /* #0B0F14 window ground (page) */
    uint32_t surface_raised; /* #121821 bars, cards, panels */
    uint32_t surface_high;   /* #18202B controls, focused rows, chips */
    uint32_t accent_dark;    /* #D7F24A tennis green: focus, primary action */
    uint32_t accent_light;   /* #6E9600 accent on a light surface (reserved) */
    uint32_t accent_pressed; /* #B4D134 pressed primary button */
    uint32_t ready;          /* #2DD4BF Ready; never the accent */
    uint32_t warning;        /* #DBB024 needs setup */
    uint32_t info;           /* #6CB6FF experimental, untested */
    uint32_t error;          /* #FF7B72 unsupported, failed, remove */
    uint32_t muted;          /* #9AA4B2 secondary text (7.6:1 on the page) */
    uint32_t focus;          /* #D7F24A focus ring */
    uint32_t line;           /* #1C232D hairlines and card edges */
    uint32_t line_strong;    /* #2E3846 control and chip borders */
    uint32_t dim;            /* #6B7684 disabled text and footnotes only */
    uint32_t pressed;        /* #222C39 pressed secondary button */
    uint32_t pressed_danger; /* #2A1A1C pressed danger button */
    uint32_t scrim;          /* page at 78% over the game */
    uint32_t art_scrim;      /* page at 72% under a badge on title art */
    uint32_t art_label;      /* #C9D6E3 labels drawn over title art */
    uint32_t placeholder;    /* #3A4552 outline of an art placeholder */
    uint32_t capture;        /* #1F2613 a row waiting for a button press */
};

/* nk_ui_theme_n1() is declared in nk_ui.h. */

/* ---- Spacing: 4 px steps ------------------------------------------------ */
#define NK_UI_SPACE_1 4.0f
#define NK_UI_SPACE_2 8.0f
#define NK_UI_SPACE_3 12.0f
#define NK_UI_SPACE_4 16.0f
#define NK_UI_SPACE_5 20.0f
#define NK_UI_SPACE_6 24.0f
#define NK_UI_SPACE_7 32.0f
#define NK_UI_SPACE_8 48.0f
#define NK_UI_SPACE_9 64.0f

/* ---- Radii --------------------------------------------------------------- */
#define NK_UI_RADIUS_CHIP 4.0f     /* status pills, badges, keycap glyphs */
#define NK_UI_RADIUS_CONTROL 8.0f  /* buttons, rows, segmented controls */
#define NK_UI_RADIUS_TILE 12.0f    /* game cards, panels, the hero */
#define NK_UI_RADIUS_DIALOG 16.0f  /* dialogs and sheets */
#define NK_UI_RADIUS_PILL 999.0f   /* fully round ends (clamped to half the height) */

/* ---- Borders, elevation and focus ---------------------------------------- */
/* Elevation is a step in surface colour plus a 1 px edge; there are no shadows.
   Level 0 page (surface), 1 card (surface_raised + line), 2 control or focused
   row (surface_high + line_strong), 3 dialog (surface_raised + line_strong over
   the scrim). */
#define NK_UI_BORDER_HAIRLINE 1.0f
#define NK_UI_FOCUS_RING_WIDTH 3.0f
#define NK_UI_FOCUS_RING_GAP 3.0f        /* between the element and the ring */
#define NK_UI_FOCUS_HC_RING_WIDTH 4.0f   /* high contrast: white ring ... */
#define NK_UI_FOCUS_HC_KEYLINE 2.0f      /* ... between black keylines */

/* ---- Control sizes -------------------------------------------------------- */
#define NK_UI_CONTROL_HEIGHT 44.0f       /* the minimum touch and pointer target */
#define NK_UI_CONTROL_HEIGHT_LARGE 52.0f
#define NK_UI_CONTROL_HEIGHT_SMALL 34.0f
#define NK_UI_CHIP_HEIGHT 26.0f
#define NK_UI_BADGE_HEIGHT 20.0f
#define NK_UI_GLYPH_SIZE 24.0f           /* prompt glyphs */

/* ---- Layout grid ---------------------------------------------------------- */
#define NK_UI_TOP_BAR_HEIGHT 64.0f
#define NK_UI_PROMPT_BAR_HEIGHT 56.0f
#define NK_UI_GUTTER 32.0f               /* page side margin; 16 below 900 px wide */
#define NK_UI_GUTTER_NARROW 16.0f
#define NK_UI_NARROW_WIDTH 900.0f
#define NK_UI_BODY_PAD_TOP 24.0f
#define NK_UI_BODY_PAD_BOTTOM 16.0f
#define NK_UI_TILE_MIN_WIDTH 236.0f      /* Library columns = how many fit */
#define NK_UI_TILE_GAP 20.0f
#define NK_UI_TILE_ROW_GAP 16.0f
#define NK_UI_TILE_MAX_COLUMNS 6u
#define NK_UI_TILE_ART_RATIO 0.53f       /* art height / tile width */
#define NK_UI_TILE_BODY_HEIGHT 90.0f     /* title (2 lines) + meta row + padding */
#define NK_UI_DETAIL_ASIDE_WIDTH 520.0f  /* Title detail: hero column */
#define NK_UI_DETAIL_GAP 32.0f
#define NK_UI_HERO_ASPECT (272.0f / 480.0f)
#define NK_UI_SAFE_AREA_MIN 16.0f        /* never closer to a window edge */

/* ---- Motion (seconds) ------------------------------------------------------ */
/* Reduce motion (prefs or the system) turns every duration into a cut. The
   waves hold one still frame; nothing in the UI ever needs motion to be read. */
#define NK_UI_MOTION_INSTANT 0.0
#define NK_UI_MOTION_FAST 0.12           /* focus ring move, button press */
#define NK_UI_MOTION_BASE 0.20           /* toast in, tab underline */
#define NK_UI_MOTION_SLOW 0.50           /* art crossfade */
#define NK_UI_MOTION_ART_REST 0.60       /* rest on a tile before its art shows */
#define NK_UI_MOTION_MUSIC_REST 1.10     /* rest before preview music starts */
#define NK_UI_MOTION_MUSIC_FADE_IN 1.00
#define NK_UI_MOTION_MUSIC_FADE_OUT 0.45
#define NK_UI_MOTION_WAVE_FRAME (1.0 / 30.0) /* waves redraw at most at 30 Hz */

/* ---- Fonts ----------------------------------------------------------------- */
/* Font slots the host registers (assets/fonts/ui, all SIL OFL 1.1). 0-3 are the
   original C2/C3 slots; 4-7 add the weights the type scale needs. */
enum {
    NK_UI_FONT_BODY = 0,           /* Geist Regular */
    NK_UI_FONT_DISPLAY = 1,        /* Chakra Petch SemiBold */
    NK_UI_FONT_MONO = 2,           /* Geist Mono Regular */
    NK_UI_FONT_CJK = 3,            /* Noto Sans JP Regular (fallback) */
    NK_UI_FONT_BODY_SEMIBOLD = 4,  /* Geist SemiBold */
    NK_UI_FONT_BODY_MEDIUM = 5,    /* Geist Medium */
    NK_UI_FONT_MONO_MEDIUM = 6,    /* Geist Mono Medium */
    NK_UI_FONT_DISPLAY_BOLD = 7,   /* Chakra Petch Bold */
    NK_UI_FONT_COUNT = 8
};

/* Relative font-file paths for each slot, indexed by the enum above. */
const char *nk_ui_font_path(int font);

/* ---- Type scale ------------------------------------------------------------ */
typedef enum {
    NK_UI_TYPE_DISPLAY_XL = 0, /* Chakra Petch 700 40/44, caps, +0.04em */
    NK_UI_TYPE_DISPLAY,        /* Chakra Petch 700 30/32, caps, +0.04em: screen names */
    NK_UI_TYPE_WORDMARK,       /* Chakra Petch 700 18, caps, +0.06em */
    NK_UI_TYPE_TITLE_XL,       /* Geist 600 30/34: a game's name on its page */
    NK_UI_TYPE_TITLE,          /* Geist 600 22/28: dialog and panel titles */
    NK_UI_TYPE_HEADING,        /* Geist 600 17/24: card headings */
    NK_UI_TYPE_TILE_TITLE,     /* Geist 600 15/20: game card names */
    NK_UI_TYPE_BODY_L,         /* Geist 400 17/26: lead paragraphs */
    NK_UI_TYPE_BODY,           /* Geist 400 15/22 */
    NK_UI_TYPE_BODY_S,         /* Geist 400 14/21: card paragraphs */
    NK_UI_TYPE_CAPTION,        /* Geist 400 13/19: hints, counts */
    NK_UI_TYPE_FOOTNOTE,       /* Geist 400 12/16, dim */
    NK_UI_TYPE_LABEL,          /* Geist Mono 500 12, caps, +0.08em: buttons, prompts */
    NK_UI_TYPE_LABEL_L,        /* Geist Mono 500 14, caps, +0.12em: the large button */
    NK_UI_TYPE_LABEL_S,        /* Geist Mono 500 11, caps, +0.06em: chips, meta rows */
    NK_UI_TYPE_EYEBROW,        /* Geist Mono 500 11, caps, +0.12em: section headers */
    NK_UI_TYPE_MICRO,          /* Geist Mono 500 10, caps, +0.08em: art badges */
    NK_UI_TYPE_DATA,           /* Geist Mono 400 12/16: disc IDs, figures */
    NK_UI_TYPE_COUNT
} NkUiTypeRole;

/* The style for one role. rgba 0 inherits the container's text colour; callers
   usually pass a theme colour. Letter spacing is already in logical px. */
NkUiTextStyle nk_ui_type(NkUiTypeRole role, uint32_t rgba);

#endif /* NK_UI_THEME_H */
