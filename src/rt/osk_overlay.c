/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * osk_overlay.c - the in-window on-screen keyboard's state machine and session (contract in
 * osk_overlay.h). Pure C: no host window, no SDL, no Win32, so it builds and tests anywhere.
 *
 * Grid: six rows of ten keys (digits, letters, symbols) and one row of five wide keys.
 * Cells are numbered 0..64: 0..59 are the ten-key rows in order, 60..64 the wide row
 * (CASE, SPACE, BKSP, CANCEL, OK, each two units wide). Navigation moves by cell order
 * within a row, and between rows by the nearest key under the cursor's centre.
 */
#include "osk_overlay.h"

#include <stdlib.h>
#include <string.h>

#define GRID_ROWS      6            /* the ten-key rows */
#define GRID_ROW_KEYS  10
#define WIDE_FIRST     60           /* index of the first wide-row cell */
#define WIDE_KEYS      5

/* Latin categories of SceUtilityOskData.inputtype, as bits of one mask. */
#define LATIN_DIGIT     OSK_INPUT_LATIN_DIGIT
#define LATIN_SYMBOL    OSK_INPUT_LATIN_SYMBOL
#define LATIN_LOWER     OSK_INPUT_LATIN_LOWERCASE
#define LATIN_UPPER     OSK_INPUT_LATIN_UPPERCASE
#define LATIN_ALL       (LATIN_DIGIT | LATIN_SYMBOL | LATIN_LOWER | LATIN_UPPER)

/* PSP button bits (src/core/nk_input_profile.h). */
#define PSP_BTN_START    0x0008u
#define PSP_BTN_UP       0x0010u
#define PSP_BTN_RIGHT    0x0020u
#define PSP_BTN_DOWN     0x0040u
#define PSP_BTN_LEFT     0x0080u
#define PSP_BTN_LTRIG    0x0100u
#define PSP_BTN_RTRIG    0x0200u
#define PSP_BTN_CIRCLE   0x2000u
#define PSP_BTN_CROSS    0x4000u

static const char s_grid[GRID_ROWS][GRID_ROW_KEYS + 1] = {
    "1234567890",
    "ABCDEFGHIJ",
    "KLMNOPQRST",
    "UVWXYZ-_.@",
    ",!?#$%&*+=",
    "/:'\"()<>~;",
};

static const OskCellKind s_wide[WIDE_KEYS] = {
    OSK_CELL_CASE, OSK_CELL_SPACE, OSK_CELL_BKSP, OSK_CELL_CANCEL, OSK_CELL_OK
};

/* ---- cell geometry -------------------------------------------------------------------------- */

static void cell_geom(int i, int *row, int *start, int *span) {
    if (i < WIDE_FIRST) {
        *row = i / GRID_ROW_KEYS;
        *start = i % GRID_ROW_KEYS;
        *span = 1;
    } else {
        *row = GRID_ROWS;
        *start = (i - WIDE_FIRST) * 2;
        *span = 2;
    }
}

static OskCellKind cell_kind(int i) {
    return i < WIDE_FIRST ? OSK_CELL_CHAR : s_wide[i - WIDE_FIRST];
}

static char cell_char(int i) {
    return i < WIDE_FIRST ? s_grid[i / GRID_ROW_KEYS][i % GRID_ROW_KEYS] : '\0';
}

/* First and last cell index of the row that holds cell i. */
static void row_bounds(int i, int *first, int *last) {
    if (i < WIDE_FIRST) {
        *first = (i / GRID_ROW_KEYS) * GRID_ROW_KEYS;
        *last = *first + GRID_ROW_KEYS - 1;
    } else {
        *first = WIDE_FIRST;
        *last = WIDE_FIRST + WIDE_KEYS - 1;
    }
}

static void row_bounds_of(int row, int *first, int *last) {
    if (row < GRID_ROWS) {
        *first = row * GRID_ROW_KEYS;
        *last = *first + GRID_ROW_KEYS - 1;
    } else {
        *first = WIDE_FIRST;
        *last = WIDE_FIRST + WIDE_KEYS - 1;
    }
}

/* ---- input type -----------------------------------------------------------------------------
 * ALL (zero) allows every Latin category. A type naming Latin categories allows exactly those.
 * A type naming no Latin category at all (Japanese, Russian, Korean, URL) cannot be shown by
 * this Latin grid, so it falls back to ALL rather than refusing every key. */

/* The low four bits of inputtype are the Latin categories. A type with any of them set is
 * restricted to those; a type with none (ALL, or only non-Latin bits) is not restricted. */
static int latin_restricted(uint32_t input_type) { return (input_type & 0xFu) != 0; }

/* The categories the grid allows: the restricted type's own bits, or LATIN_ALL for a type that
 * is not restricted (the fallback described above). */
static uint32_t latin_bits(uint32_t input_type) {
    return latin_restricted(input_type) ? (input_type & 0xFu) : LATIN_ALL;
}

static int letters_allowed(uint32_t bits) { return (bits & (LATIN_LOWER | LATIN_UPPER)) != 0; }
static int case_toggle_allowed(uint32_t bits) {
    return (bits & LATIN_LOWER) && (bits & LATIN_UPPER);
}

/* Letters are typed in lower case when lower case is the only allowed case, or when both
 * are allowed and the person has switched to it. */
static int letter_lower(const OskOverlay *o) {
    uint32_t bits = latin_bits(o->input_type);
    if (!(bits & LATIN_UPPER)) return 1;
    if (!(bits & LATIN_LOWER)) return 0;
    return o->lower;
}

static int cell_enabled(const OskOverlay *o, int i) {
    uint32_t bits = latin_bits(o->input_type);
    if (i >= WIDE_FIRST) {
        return cell_kind(i) != OSK_CELL_CASE || case_toggle_allowed(bits);
    }
    char c = cell_char(i);
    if (c >= '0' && c <= '9') return (bits & LATIN_DIGIT) != 0;
    if (c >= 'A' && c <= 'Z') return letters_allowed(bits);
    return (bits & LATIN_SYMBOL) != 0;
}

/* ---- text -----------------------------------------------------------------------------------*/

static int is_high(uint16_t u) { return u >= 0xD800u && u <= 0xDBFFu; }
static int is_low(uint16_t u) { return u >= 0xDC00u && u <= 0xDFFFu; }

static int append_cp(OskOverlay *o, uint32_t cp) {
    int units = cp >= 0x10000u ? 2 : 1;
    int max = o->cap - 1;
    if (o->len + units > max) return 0;
    if (units == 1) {
        o->text[o->len++] = (uint16_t)cp;
    } else {
        cp -= 0x10000u;
        o->text[o->len++] = (uint16_t)(0xD800u + (cp >> 10));
        o->text[o->len++] = (uint16_t)(0xDC00u + (cp & 0x3FFu));
    }
    o->text[o->len] = 0;
    return 1;
}

static int backspace(OskOverlay *o) {
    int n = 1;
    if (o->len <= 0) return 0;
    if (o->len >= 2 && is_low(o->text[o->len - 1]) && is_high(o->text[o->len - 2])) n = 2;
    o->len -= n;
    o->text[o->len] = 0;
    return 1;
}

/* ---- navigation ------------------------------------------------------------------------------ */

/* Both moves return 1 when the cursor moved, 0 when the grid edge stopped it. */
static int move_horizontal(OskOverlay *o, int dir) {
    int first, last;
    row_bounds(o->cursor, &first, &last);
    for (int j = o->cursor + dir; j >= first && j <= last; j += dir) {
        if (cell_enabled(o, j)) {
            o->cursor = j;
            return 1;
        }
    }
    return 0;
}

static int move_vertical(OskOverlay *o, int dir) {
    int row, start, span;
    cell_geom(o->cursor, &row, &start, &span);
    /* Centres are doubled so they stay integers: a ten-key cell (span 1) centres at start + 0.5
     * and a wide key (span 2) at start + 1, so 2 * start + span is the exact centre in both. */
    int centre = 2 * start + span;
    for (int r = row + dir; r >= 0 && r <= GRID_ROWS; r += dir) {
        int first, last, best = -1, best_gap = 1 << 30;
        row_bounds_of(r, &first, &last);
        for (int j = first; j <= last; j++) {
            if (!cell_enabled(o, j)) continue;
            int jr, js, jspan;
            cell_geom(j, &jr, &js, &jspan);
            int gap = abs(2 * js + jspan - centre);
            if (gap < best_gap) {
                best_gap = gap;
                best = j;
            }
        }
        if (best >= 0) {
            o->cursor = best;
            return 1;
        }
    }
    return 0;
}

/* ---- keys ------------------------------------------------------------------------------------ */

static int act_on_cursor(OskOverlay *o) {
    int i = o->cursor;
    if (!cell_enabled(o, i)) return 0;
    switch (cell_kind(i)) {
    case OSK_CELL_CHAR: {
        char c = cell_char(i);
        if (c >= 'A' && c <= 'Z' && letter_lower(o)) c = (char)(c - 'A' + 'a');
        return append_cp(o, (uint32_t)(unsigned char)c);
    }
    case OSK_CELL_SPACE:
        return append_cp(o, ' ');
    case OSK_CELL_BKSP:
        return backspace(o);
    case OSK_CELL_CASE:
        o->lower = !o->lower;
        return 1;
    case OSK_CELL_CANCEL:
        o->status = OSK_OVERLAY_CANCELLED;
        return 1;
    case OSK_CELL_OK:
        o->status = OSK_OVERLAY_CONFIRMED;
        return 1;
    }
    return 0;
}

void osk_overlay_reset(OskOverlay *o) {
    memset(o, 0, sizeof *o);
}

void osk_overlay_open(OskOverlay *o, const uint16_t *desc, const uint16_t *initial, int cap,
                      uint32_t input_type) {
    /* The held pad state survives an open: a button already down does not act on the new
     * request, only a press made after it opens does. */
    uint32_t held = o->pad_prev;
    osk_overlay_reset(o);
    o->pad_prev = held;
    if (cap < 2) cap = 2;
    if (cap > OSK_OVERLAY_TEXT_MAX) cap = OSK_OVERLAY_TEXT_MAX;
    o->cap = cap;
    o->input_type = input_type;
    if (desc) {
        int n = 0;
        while (n < OSK_OVERLAY_DESC_MAX - 1 && desc[n]) {
            o->desc[n] = desc[n];
            n++;
        }
        o->desc[n] = 0;
    }
    if (initial) {
        for (int n = 0; n < cap - 1 && initial[n]; n++) o->text[o->len++] = initial[n];
        o->text[o->len] = 0;
    }
    o->status = OSK_OVERLAY_OPEN;
    /* Start on the first enabled key, preferring the letter A, as the grid reads. */
    o->cursor = 0;
    if (letters_allowed(latin_bits(input_type))) o->cursor = GRID_ROW_KEYS;
    if (!cell_enabled(o, o->cursor)) {
        for (int i = 0; i < WIDE_FIRST + WIDE_KEYS; i++) {
            if (cell_enabled(o, i)) {
                o->cursor = i;
                break;
            }
        }
    }
}

int osk_overlay_press(OskOverlay *o, OskKey key) {
    if (o->status != OSK_OVERLAY_OPEN) return 0;
    switch (key) {
    case OSK_KEY_UP:       return move_vertical(o, -1);
    case OSK_KEY_DOWN:     return move_vertical(o, +1);
    case OSK_KEY_LEFT:     return move_horizontal(o, -1);
    case OSK_KEY_RIGHT:    return move_horizontal(o, +1);
    case OSK_KEY_SELECT:   return act_on_cursor(o);
    case OSK_KEY_CANCEL:   o->status = OSK_OVERLAY_CANCELLED; return 1;
    case OSK_KEY_CONFIRM:  o->status = OSK_OVERLAY_CONFIRMED; return 1;
    case OSK_KEY_BACKSPACE: return backspace(o);
    case OSK_KEY_CASE:
        if (!case_toggle_allowed(latin_bits(o->input_type))) return 0;
        o->lower = !o->lower;
        return 1;
    }
    return 0;
}

int osk_overlay_allows(const OskOverlay *o, uint32_t cp) {
    uint32_t bits = latin_bits(o->input_type);
    if (cp == ' ') return 1;
    if (cp < 0x20u || cp == 0x7Fu || (cp >= 0xD800u && cp <= 0xDFFFu) || cp > 0x10FFFFu)
        return 0;
    /* The grid has no non-ASCII key, so only a type with no Latin category named can type it. */
    if (cp >= 0x80u) return !latin_restricted(o->input_type);
    if (cp >= '0' && cp <= '9') return (bits & LATIN_DIGIT) != 0;
    if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z')) return letters_allowed(bits);
    return (bits & LATIN_SYMBOL) != 0;
}

int osk_overlay_type(OskOverlay *o, uint32_t cp) {
    if (o->status != OSK_OVERLAY_OPEN) return 0;
    if (!osk_overlay_allows(o, cp)) return 0;
    uint32_t bits = latin_bits(o->input_type);
    /* Where both cases are allowed a typed letter keeps its case; where only one is, it is
     * forced to that case. */
    if (cp >= 'A' && cp <= 'Z' && !(bits & LATIN_UPPER)) cp = cp - 'A' + 'a';
    else if (cp >= 'a' && cp <= 'z' && !(bits & LATIN_LOWER)) cp = cp - 'a' + 'A';
    return append_cp(o, cp);
}

void osk_overlay_pad(OskOverlay *o, uint32_t scripted, uint32_t pulses) {
    static const struct { uint32_t bit; OskKey key; } map[] = {
        { PSP_BTN_UP, OSK_KEY_UP },       { PSP_BTN_DOWN, OSK_KEY_DOWN },
        { PSP_BTN_LEFT, OSK_KEY_LEFT },   { PSP_BTN_RIGHT, OSK_KEY_RIGHT },
        { PSP_BTN_CROSS, OSK_KEY_SELECT }, { PSP_BTN_CIRCLE, OSK_KEY_CANCEL },
        { PSP_BTN_START, OSK_KEY_CONFIRM }, { PSP_BTN_LTRIG, OSK_KEY_CASE },
        { PSP_BTN_RTRIG, OSK_KEY_CASE },
    };
    uint32_t rising = (scripted & ~o->pad_prev) | pulses;
    o->pad_prev = scripted;
    if (o->status != OSK_OVERLAY_OPEN) return;
    for (size_t k = 0; k < sizeof map / sizeof map[0]; k++) {
        if (!(rising & map[k].bit)) continue;
        (void)osk_overlay_press(o, map[k].key);
        if (o->status != OSK_OVERLAY_OPEN) return;
    }
}

int osk_overlay_answer(const OskOverlay *o, uint16_t *out, int cap) {
    if (o->status != OSK_OVERLAY_CONFIRMED) return -1;
    if (!out || cap < 1) return 0;
    int n = o->len < cap - 1 ? o->len : cap - 1;
    if (n > 0 && is_high(o->text[n - 1])) n--;      /* never split a surrogate pair */
    memcpy(out, o->text, (size_t)n * sizeof out[0]);
    out[n] = 0;
    return n;
}

int osk_overlay_cells(const OskOverlay *o, OskCell *out, int max) {
    int count = 0;
    for (int i = 0; i < OSK_OVERLAY_CELLS && count < max; i++, count++) {
        OskCell *c = &out[count];
        cell_geom(i, &c->row, &c->start, &c->span);
        c->kind = cell_kind(i);
        c->ch = cell_char(i);
        if (c->kind == OSK_CELL_CHAR && c->ch >= 'A' && c->ch <= 'Z' && letter_lower(o))
            c->ch = (char)(c->ch - 'A' + 'a');
        c->enabled = cell_enabled(o, i);
        c->highlight = i == o->cursor;
    }
    return count;
}

/* ---- session --------------------------------------------------------------------------------
 * One keyboard per process, like the other runtime services. Scheduler thread only. */

static OskOverlay s_session;
static int s_host;

/* Return the session to IDLE but keep the held pad state, which tracks the pad whether or not
 * a request is open: a button still down after an answer must not act on the next field. */
static void session_idle(void) {
    uint32_t held = s_session.pad_prev;
    osk_overlay_reset(&s_session);
    s_session.pad_prev = held;
}

void sr_osk_overlay_set_host(int on) { s_host = on != 0; }
int  sr_osk_overlay_host(void) { return s_host; }

static uint32_t wide_to_u16(const wchar_t *src, uint16_t *dst, int max) {
    int n = 0;
    if (src)
        while (n < max - 1 && src[n]) {
            dst[n] = (uint16_t)src[n];
            n++;
        }
    dst[n] = 0;
    return (uint32_t)n;
}

int sr_osk_overlay_poll(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap,
                        uint32_t input_type) {
    if (!out || cap < 2) return 0;
    switch (s_session.status) {
    case OSK_OVERLAY_OPEN:
        return SR_OSK_TEXT_PENDING;
    case OSK_OVERLAY_CONFIRMED: {
        uint16_t text[OSK_OVERLAY_TEXT_MAX];
        int limit = cap < OSK_OVERLAY_TEXT_MAX ? cap : OSK_OVERLAY_TEXT_MAX;
        int n = osk_overlay_answer(&s_session, text, limit);
        for (int i = 0; i < n; i++) out[i] = (wchar_t)text[i];
        out[n] = 0;
        session_idle();
        return 1;
    }
    case OSK_OVERLAY_CANCELLED:
        session_idle();
        return 0;
    case OSK_OVERLAY_IDLE:
    default: {
        uint16_t d[OSK_OVERLAY_DESC_MAX], t[OSK_OVERLAY_TEXT_MAX];
        wide_to_u16(desc, d, OSK_OVERLAY_DESC_MAX);
        wide_to_u16(initial, t, OSK_OVERLAY_TEXT_MAX);
        osk_overlay_open(&s_session, d, t, cap, input_type);
        return SR_OSK_TEXT_PENDING;
    }
    }
}

void sr_osk_overlay_abandon(void) { session_idle(); }

int sr_osk_overlay_active(void) { return s_session.status == OSK_OVERLAY_OPEN; }

void sr_osk_overlay_pad(uint32_t scripted, uint32_t pulses) {
    osk_overlay_pad(&s_session, scripted, pulses);
}

void sr_osk_overlay_key(OskKey key) { (void)osk_overlay_press(&s_session, key); }

void sr_osk_overlay_type(uint32_t codepoint) { (void)osk_overlay_type(&s_session, codepoint); }

const OskOverlay *sr_osk_overlay_view(void) { return &s_session; }
