/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * osk_overlay.h - the in-window on-screen keyboard: one field's text, its cursor and its
 * answer, driven by the same non-blocking request as the native box (osk_text_entry.h).
 *
 * The keyboard is a grid of keys: digits, letters, symbols, then a row with CASE, SPACE,
 * BKSP, CANCEL and OK. A person moves the highlight (d-pad, arrow keys), types the
 * highlighted key (Cross, or Enter on a keyboard) and leaves with Start (OK) or Circle
 * (CANCEL). A physical keyboard also types directly. Every answer is a UTF-16 string of at
 * most cap - 1 units, where cap is the guest's own buffer size (the OSK parameters bound it),
 * and the field's inputtype (SceUtilityOskData.inputtype, public PSPSDK layout) limits which
 * Latin characters the grid and the keyboard accept.
 *
 * Two layers live here:
 *   - the instance API (osk_overlay_*) is pure C with no host dependency, so the state
 *     machine can be tested without a window;
 *   - the session API (sr_osk_overlay_*) is the one process-wide keyboard the runtime uses.
 *     Its owner is the scheduler thread: sr_ctrl_sample() (the vblank controller latch) feeds
 *     the pad, the presenter (gui.c) feeds keyboard events and draws the frame, and the HLE
 *     OSK polls the answer. Nothing here waits, so guest time keeps running while it is open.
 *
 * Pad input is edge-based. A button is one press when it goes down, however long it is held,
 * so a held d-pad does not race through the grid. The scripted route (SR_PADSCRIPT) reaches
 * the keyboard as its mask, and the live gamepad as press events. The auto-START pulse never
 * does, so a headless run cannot confirm a name it never typed.
 */
#ifndef SR_OSK_OVERLAY_H
#define SR_OSK_OVERLAY_H

#include <stdint.h>

#include "osk_text_entry.h" /* SR_OSK_TEXT_PENDING and the answer contract */

#define OSK_OVERLAY_TEXT_MAX 256 /* UTF-16 units held, terminator included (the guest bound) */
#define OSK_OVERLAY_DESC_MAX 128 /* description units held, terminator included */

/* SceUtilityOskData.inputtype bits (public PSPSDK psputility_osk.h). ALL is zero. */
#define OSK_INPUT_LATIN_DIGIT     0x00000001u
#define OSK_INPUT_LATIN_SYMBOL    0x00000002u
#define OSK_INPUT_LATIN_LOWERCASE 0x00000004u
#define OSK_INPUT_LATIN_UPPERCASE 0x00000008u

typedef enum {
    OSK_KEY_UP,
    OSK_KEY_DOWN,
    OSK_KEY_LEFT,
    OSK_KEY_RIGHT,
    OSK_KEY_SELECT,    /* type the highlighted key (Cross, Enter) */
    OSK_KEY_CANCEL,    /* leave without an answer (Circle, Esc) */
    OSK_KEY_CONFIRM,   /* answer with the text so far (Start, Tab) */
    OSK_KEY_BACKSPACE, /* delete the last character (keyboard Backspace) */
    OSK_KEY_CASE       /* switch letter case where both cases are allowed (L, R) */
} OskKey;

typedef enum {
    OSK_CELL_CHAR,    /* a digit, letter or symbol: label is ch */
    OSK_CELL_SPACE,
    OSK_CELL_BKSP,
    OSK_CELL_CASE,
    OSK_CELL_CANCEL,
    OSK_CELL_OK
} OskCellKind;

/* One key of the grid, as the presenter draws it. Keys occupy `span` units of a 10-unit row. */
typedef struct {
    int         row;
    int         start;
    int         span;
    OskCellKind kind;
    char        ch;        /* CELL_CHAR: the key's character in the current letter case */
    int         enabled;   /* 0: this input type does not allow the key (drawn dimmed) */
    int         highlight; /* 1: the cursor is on this key */
} OskCell;

#define OSK_OVERLAY_ROWS  7
#define OSK_OVERLAY_CELLS 65

typedef enum {
    OSK_OVERLAY_IDLE,      /* no request */
    OSK_OVERLAY_OPEN,      /* waiting for the person */
    OSK_OVERLAY_CONFIRMED, /* answered with text; the next poll hands it over */
    OSK_OVERLAY_CANCELLED  /* answered cancelled; the next poll reports 0 */
} OskOverlayStatus;

typedef struct {
    OskOverlayStatus status;
    int      cap;                            /* the guest buffer, units incl. terminator */
    uint32_t input_type;                     /* SceUtilityOskData.inputtype */
    uint16_t desc[OSK_OVERLAY_DESC_MAX];     /* the guest's description, UTF-16 */
    uint16_t text[OSK_OVERLAY_TEXT_MAX];     /* the text so far, UTF-16, NUL-terminated */
    int      len;                            /* units in text, terminator excluded */
    int      cursor;                         /* index of the highlighted cell */
    int      lower;                          /* letters are typed in lower case */
    uint32_t pad_prev;                       /* scripted pad bits seen by the last update */
} OskOverlay;

/* ---- instance API: no host dependency ---------------------------------------------------- */

/* Clear to IDLE. Every other call requires an instance that was reset or opened before. */
void osk_overlay_reset(OskOverlay *o);

/* Open a request: copy the description and the initial text (truncated to cap - 1 units),
 * highlight the first letter, and wait. cap < 2 is clamped to 2; cap is capped at the
 * guest's bound. */
void osk_overlay_open(OskOverlay *o, const uint16_t *desc, const uint16_t *initial, int cap,
                      uint32_t input_type);

/* Apply one key while open. Returns 1 when the state changed. Keys are ignored when the
 * request is not open. */
int osk_overlay_press(OskOverlay *o, OskKey key);

/* Type one code point (a physical keyboard's character). Returns 1 when it was appended.
 * Refused: control characters, lone surrogates, out-of-range values, characters the
 * input type excludes, and anything that would not fit whole (a supplementary character
 * needs two free units). */
int osk_overlay_type(OskOverlay *o, uint32_t codepoint);

/* Feed the pad. `scripted` is the mask a route or a test holds this vblank; `pulses` are
 * buttons that went down since the last feed (live gamepad presses, which can come and go
 * between samples). Every rising edge of either applies one key: d-pad moves, Cross
 * selects, Circle cancels, Start confirms, L/R switch case. Called every vblank, whether
 * or not the request is open, so the held state stays current. */
void osk_overlay_pad(OskOverlay *o, uint32_t scripted, uint32_t pulses);

/* Copy the answer (confirmed text) into the guest's buffer, at most cap - 1 units and a
 * terminator. Returns the units written, or -1 when the request was not confirmed. */
int osk_overlay_answer(const OskOverlay *o, uint16_t *out, int cap);

/* The grid as drawn now: fills up to max entries and returns how many there are. */
int osk_overlay_cells(const OskOverlay *o, OskCell *out, int max);

/* Whether the input type allows this code point (osk_overlay_type's rule, exposed for
 * tests). */
int osk_overlay_allows(const OskOverlay *o, uint32_t codepoint);

/* ---- session API: the process-wide keyboard -------------------------------------------- */

/* 1 when the presenter can draw the keyboard, so the native box is no longer the only
 * way to answer a field. gui.c sets it once the SDL3 presenter is up. */
void sr_osk_overlay_set_host(int on);
int  sr_osk_overlay_host(void);

/* The poll that osk_text_entry.c forwards to in overlay mode: opens a request on the first
 * call and returns SR_OSK_TEXT_PENDING until the person answers, then 1 (text in out) or 0
 * (cancelled). Same contract as sr_osk_text_entry_poll. */
int  sr_osk_overlay_poll(const wchar_t *desc, const wchar_t *initial, wchar_t *out, int cap,
                         uint32_t input_type);
void sr_osk_overlay_abandon(void);

/* 1 while a request is open and waiting for the person. */
int  sr_osk_overlay_active(void);

/* Pad and keyboard input for the session (no-ops while nothing is open, except that the pad
 * still tracks its held state). */
void sr_osk_overlay_pad(uint32_t scripted, uint32_t pulses);
void sr_osk_overlay_key(OskKey key);
void sr_osk_overlay_type(uint32_t codepoint);

/* The session as the presenter draws it. */
const OskOverlay *sr_osk_overlay_view(void);

#endif /* SR_OSK_OVERLAY_H */
