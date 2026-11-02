/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * osk_overlay_selftest.c - the in-window keyboard's state machine (src/rt/osk_overlay.c):
 * navigation, typing, the guest's length bound, backspace, confirm and cancel, the UTF-16
 * answer, the input-type rules, edge-based pad input, and the session contract the HLE
 * keyboard polls. Pure C, no window, no game data.
 *
 * Cell numbers follow osk_overlay.c: rows of ten (0..59) then the wide row (60 CASE,
 * 61 SPACE, 62 BKSP, 63 CANCEL, 64 OK). 'A' is cell 10, 'K' is 20, 'U' is 30, ',' is 40,
 * '/' is 50.
 */
#include "osk_overlay.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

static int s_checks;
static int s_failures;

#define CHECK(cond, msg)                                                  \
    do {                                                                  \
        s_checks++;                                                       \
        if (!(cond)) {                                                    \
            s_failures++;                                                 \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); \
        }                                                                 \
    } while (0)

#define CELL_A      10
#define CELL_B      11
#define CELL_K      20
#define CELL_U      30
#define CELL_CASE   60
#define CELL_SPACE  61
#define CELL_CANCEL 63
#define CELL_OK     64

/* PSP button bits, as the pad feeds them. */
#define PAD_START  0x0008u
#define PAD_UP     0x0010u
#define PAD_RIGHT  0x0020u
#define PAD_DOWN   0x0040u
#define PAD_LEFT   0x0080u
#define PAD_LTRIG  0x0100u
#define PAD_CIRCLE 0x2000u
#define PAD_CROSS  0x4000u

/* Separate slots, so one call can take two strings (description and initial text). */
static uint16_t s_u16[3][OSK_OVERLAY_TEXT_MAX];

/* NUL-terminated UTF-16 from ASCII (test input only). */
static const uint16_t *units(int slot, const char *ascii) {
    int n = 0;
    while (ascii[n] && n < OSK_OVERLAY_TEXT_MAX - 1) {
        s_u16[slot][n] = (uint16_t)(unsigned char)ascii[n];
        n++;
    }
    s_u16[slot][n] = 0;
    return s_u16[slot];
}

static int text_is(const OskOverlay *o, const char *ascii) {
    int n = (int)strlen(ascii);
    if (o->len != n) return 0;
    for (int i = 0; i < n; i++)
        if (o->text[i] != (uint16_t)(unsigned char)ascii[i]) return 0;
    return o->text[n] == 0;
}

static int highlighted_is(const OskOverlay *o, char c) {
    OskCell cells[OSK_OVERLAY_CELLS];
    int n = osk_overlay_cells(o, cells, OSK_OVERLAY_CELLS);
    for (int i = 0; i < n; i++)
        if (cells[i].highlight) return cells[i].kind == OSK_CELL_CHAR && cells[i].ch == c;
    return 0;
}

static int enabled_count(const OskOverlay *o) {
    OskCell cells[OSK_OVERLAY_CELLS];
    int n = osk_overlay_cells(o, cells, OSK_OVERLAY_CELLS), count = 0;
    for (int i = 0; i < n; i++) count += cells[i].enabled;
    return count;
}

static void test_open_and_cells(void) {
    OskOverlay o;
    osk_overlay_reset(&o);
    osk_overlay_open(&o, units(0, "Enter name."), units(1, "old"), 9, 0);
    CHECK(o.status == OSK_OVERLAY_OPEN, "open: the request waits for the person");
    CHECK(o.desc[0] == 'E' && o.desc[10] == '.' && o.desc[11] == 0,
          "open: the description is copied");
    CHECK(text_is(&o, "old"), "open: the initial text is the field's current text");
    CHECK(o.cursor == CELL_A && highlighted_is(&o, 'A'), "open: the cursor starts on A");
    CHECK(enabled_count(&o) == OSK_OVERLAY_CELLS, "open: every key is enabled under ALL");

    OskCell cells[OSK_OVERLAY_CELLS];
    int n = osk_overlay_cells(&o, cells, OSK_OVERLAY_CELLS), lit = 0;
    for (int i = 0; i < n; i++) lit += cells[i].highlight;
    CHECK(n == OSK_OVERLAY_CELLS && lit == 1, "cells: 65 keys, exactly one highlighted");
    CHECK(osk_overlay_cells(&o, cells, 3) == 3, "cells: a short buffer is filled and counted");

    osk_overlay_open(&o, units(0, "x"), units(1, "abcdef"), 4, 0);
    CHECK(text_is(&o, "abc"), "open: initial text is cut to the cap less the terminator");
    osk_overlay_open(&o, NULL, NULL, 0, 0);
    CHECK(o.cap == 2, "open: a cap below two is raised to two");
}

static void test_navigation(void) {
    OskOverlay o;
    osk_overlay_reset(&o);
    osk_overlay_open(&o, NULL, NULL, 8, 0);
    CHECK(osk_overlay_press(&o, OSK_KEY_RIGHT) && o.cursor == CELL_B, "right moves to B");
    CHECK(osk_overlay_press(&o, OSK_KEY_LEFT) && o.cursor == CELL_A, "left moves back to A");
    CHECK(osk_overlay_press(&o, OSK_KEY_LEFT) == 0 && o.cursor == CELL_A,
          "left at the row's first key stays put");
    CHECK(osk_overlay_press(&o, OSK_KEY_DOWN) && o.cursor == CELL_K, "down moves to K");
    CHECK(osk_overlay_press(&o, OSK_KEY_UP) && o.cursor == CELL_A, "up returns to A");
    CHECK(osk_overlay_press(&o, OSK_KEY_UP) && o.cursor == 0, "up reaches the digit row (1)");
    CHECK(osk_overlay_press(&o, OSK_KEY_UP) == 0 && o.cursor == 0, "up at the top row stays put");

    /* Four downs from A reach row 5; the fifth reaches the wide row at CASE. */
    osk_overlay_open(&o, NULL, NULL, 8, 0);
    for (int i = 0; i < 4; i++) osk_overlay_press(&o, OSK_KEY_DOWN);
    CHECK(o.cursor == 50, "four downs from A reach row 5, column 0 ('/')");
    CHECK(osk_overlay_press(&o, OSK_KEY_DOWN) && o.cursor == CELL_CASE,
          "down from row 5, column 0 lands on the key under the cursor (CASE)");
    CHECK(osk_overlay_press(&o, OSK_KEY_RIGHT) && o.cursor == CELL_SPACE,
          "right on the wide row moves to SPACE");
    for (int i = 0; i < 3; i++) osk_overlay_press(&o, OSK_KEY_RIGHT);
    CHECK(o.cursor == CELL_OK, "right three more times reaches OK");
    CHECK(osk_overlay_press(&o, OSK_KEY_RIGHT) == 0 && o.cursor == CELL_OK,
          "right at the wide row's last key stays put");
    CHECK(osk_overlay_press(&o, OSK_KEY_UP) && o.cursor == 58,
          "up from OK lands on the nearest key above it ('~')");

    osk_overlay_open(&o, NULL, NULL, 8, 0);
    for (int i = 0; i < 5; i++) osk_overlay_press(&o, OSK_KEY_DOWN);
    osk_overlay_press(&o, OSK_KEY_RIGHT);
    CHECK(o.cursor == CELL_SPACE, "SPACE is reached from CASE");
    CHECK(osk_overlay_press(&o, OSK_KEY_UP) && o.cursor == 52,
          "up from SPACE lands on the nearest row-5 key ('\\'')");
}

static void test_typing_and_case(void) {
    OskOverlay o;
    osk_overlay_reset(&o);
    osk_overlay_open(&o, NULL, NULL, 16, 0);
    CHECK(osk_overlay_press(&o, OSK_KEY_SELECT) && text_is(&o, "A"), "select types A");
    osk_overlay_press(&o, OSK_KEY_RIGHT);
    CHECK(osk_overlay_press(&o, OSK_KEY_CASE) && o.lower == 1, "case switches to lower case");
    CHECK(osk_overlay_press(&o, OSK_KEY_SELECT) && text_is(&o, "Ab"),
          "with lower case selected, B is typed as b");
    osk_overlay_press(&o, OSK_KEY_CASE);
    CHECK(osk_overlay_press(&o, OSK_KEY_SELECT) && text_is(&o, "AbB"),
          "case switches back to upper, and B is typed as B");
    CHECK(osk_overlay_type(&o, '7') && text_is(&o, "AbB7"), "a typed digit is appended");
    CHECK(osk_overlay_type(&o, 'q') && text_is(&o, "AbB7q"),
          "a typed lower-case letter keeps its case under ALL");
    CHECK(osk_overlay_type(&o, 'Q') && text_is(&o, "AbB7qQ"),
          "a typed upper-case letter keeps its case under ALL");
    CHECK(osk_overlay_press(&o, OSK_KEY_BACKSPACE) && text_is(&o, "AbB7q"),
          "backspace deletes one unit");
    CHECK(osk_overlay_type(&o, ' ') && text_is(&o, "AbB7q "), "a space is typed");
    CHECK(osk_overlay_type(&o, 0x0Au) == 0 && text_is(&o, "AbB7q "),
          "a control character is refused");
    CHECK(osk_overlay_type(&o, 0xD800u) == 0, "a lone surrogate is refused");
    CHECK(osk_overlay_type(&o, 0x110000u) == 0, "a code point above U+10FFFF is refused");
}

static void test_length_limit_and_unicode(void) {
    OskOverlay o;
    osk_overlay_reset(&o);
    /* cap 4: three units and the terminator. */
    osk_overlay_open(&o, NULL, NULL, 4, 0);
    CHECK(osk_overlay_type(&o, 'A') && osk_overlay_type(&o, 'B') && osk_overlay_type(&o, 'C'),
          "three characters fit a four-unit buffer");
    CHECK(osk_overlay_type(&o, 'D') == 0 && text_is(&o, "ABC"),
          "the fourth character is refused");
    CHECK(osk_overlay_press(&o, OSK_KEY_SELECT) == 0 && text_is(&o, "ABC"),
          "select on a full field adds nothing");

    /* cap 3: two units. A supplementary character (two units) does not fit after one unit. */
    osk_overlay_open(&o, NULL, NULL, 3, 0);
    CHECK(osk_overlay_type(&o, 'x'), "one unit is used");
    CHECK(osk_overlay_type(&o, 0x1F600u) == 0 && text_is(&o, "x"),
          "a two-unit character is refused whole when one unit is free");

    /* cap 4: U+1F600 is D83D DE00; U+00E9 is one unit. */
    osk_overlay_open(&o, NULL, NULL, 4, 0);
    CHECK(osk_overlay_type(&o, 0x1F600u) && o.len == 2, "a supplementary character takes two units");
    CHECK(o.text[0] == 0xD83Du && o.text[1] == 0xDE00u,
          "U+1F600 is encoded as the UTF-16 surrogate pair D83D DE00");
    CHECK(osk_overlay_type(&o, 0x00E9u) && o.len == 3 && o.text[2] == 0x00E9u,
          "a BMP non-ASCII character is one unit");
    CHECK(osk_overlay_type(&o, 'z') == 0 && o.len == 3, "the buffer stays bounded");
    CHECK(osk_overlay_press(&o, OSK_KEY_BACKSPACE) && o.len == 2 && o.text[0] == 0xD83Du,
          "backspace removes the one-unit character first");
    CHECK(osk_overlay_press(&o, OSK_KEY_BACKSPACE) && o.len == 0,
          "backspace removes the whole surrogate pair");
    CHECK(osk_overlay_press(&o, OSK_KEY_BACKSPACE) == 0, "backspace on an empty field does nothing");
}

static void test_cancel_confirm_and_answer(void) {
    OskOverlay o;
    uint16_t out[8];
    osk_overlay_reset(&o);

    osk_overlay_open(&o, NULL, units(0, "ab"), 8, 0);
    osk_overlay_type(&o, 'c');
    CHECK(osk_overlay_answer(&o, out, 8) == -1, "no answer while the request is open");
    CHECK(osk_overlay_press(&o, OSK_KEY_CONFIRM) && o.status == OSK_OVERLAY_CONFIRMED,
          "confirm answers with the text so far");
    CHECK(osk_overlay_press(&o, OSK_KEY_CONFIRM) == 0, "a second confirm does nothing");
    int n = osk_overlay_answer(&o, out, 8);
    CHECK(n == 3 && out[0] == 'a' && out[1] == 'b' && out[2] == 'c' && out[3] == 0,
          "the answer is the UTF-16 text with a terminator");

    osk_overlay_open(&o, NULL, NULL, 8, 0);
    osk_overlay_press(&o, OSK_KEY_CANCEL);
    CHECK(o.status == OSK_OVERLAY_CANCELLED, "cancel leaves without an answer");
    CHECK(osk_overlay_answer(&o, out, 8) == -1, "a cancelled request has no answer");

    osk_overlay_open(&o, NULL, NULL, 8, 0);
    o.cursor = CELL_OK;
    CHECK(osk_overlay_press(&o, OSK_KEY_SELECT) && o.status == OSK_OVERLAY_CONFIRMED,
          "selecting OK on the grid confirms");
    osk_overlay_open(&o, NULL, NULL, 8, 0);
    o.cursor = CELL_CANCEL;
    CHECK(osk_overlay_press(&o, OSK_KEY_SELECT) && o.status == OSK_OVERLAY_CANCELLED,
          "selecting CANCEL on the grid cancels");

    /* The answer never exceeds the caller's buffer and never splits a surrogate pair. */
    osk_overlay_open(&o, NULL, NULL, 8, 0);
    osk_overlay_type(&o, 0x1F600u);
    osk_overlay_type(&o, 'z');
    osk_overlay_press(&o, OSK_KEY_CONFIRM);
    CHECK(osk_overlay_answer(&o, out, 2) == 0 && out[0] == 0,
          "a pair is not split into a one-unit buffer");
    CHECK(osk_overlay_answer(&o, out, 3) == 2 && out[0] == 0xD83Du && out[1] == 0xDE00u,
          "a three-unit buffer takes the pair");
    CHECK(osk_overlay_answer(&o, out, 4) == 3 && out[2] == 'z',
          "a four-unit buffer takes the pair and the z");
}

static void test_pad_edges(void) {
    OskOverlay o;
    osk_overlay_reset(&o);
    osk_overlay_open(&o, NULL, NULL, 8, 0);

    osk_overlay_pad(&o, PAD_DOWN, 0);
    CHECK(o.cursor == CELL_K, "a pressed d-pad moves once");
    osk_overlay_pad(&o, PAD_DOWN, 0);
    CHECK(o.cursor == CELL_K, "a held d-pad does not repeat");
    osk_overlay_pad(&o, 0, 0);
    osk_overlay_pad(&o, PAD_DOWN, 0);
    CHECK(o.cursor == CELL_U, "releasing and pressing again moves again");

    osk_overlay_pad(&o, 0, PAD_RIGHT);
    CHECK(o.cursor == CELL_U + 1, "a pulse from a fast press moves once");
    osk_overlay_pad(&o, 0, 0);
    CHECK(o.cursor == CELL_U + 1, "a pulse is not repeated by the next feed");

    osk_overlay_pad(&o, 0, PAD_CROSS);
    CHECK(text_is(&o, "V"), "cross types the highlighted key (V)");
    osk_overlay_pad(&o, 0, PAD_CIRCLE);
    CHECK(o.status == OSK_OVERLAY_CANCELLED, "circle cancels");

    osk_overlay_open(&o, NULL, NULL, 8, 0);
    osk_overlay_pad(&o, PAD_START, 0);
    CHECK(o.status == OSK_OVERLAY_CONFIRMED, "start confirms");

    /* A button held down when a request opens does not act on it. */
    osk_overlay_reset(&o);
    osk_overlay_pad(&o, PAD_START, 0);
    osk_overlay_open(&o, NULL, NULL, 8, 0);
    osk_overlay_pad(&o, PAD_START, 0);
    CHECK(o.status == OSK_OVERLAY_OPEN, "a Start held at open does not confirm the request");
    osk_overlay_pad(&o, 0, 0);
    osk_overlay_pad(&o, PAD_START, 0);
    CHECK(o.status == OSK_OVERLAY_CONFIRMED, "a fresh Start press confirms");

    /* The held state is tracked while closed, so a d-pad held across the open does not move. */
    osk_overlay_reset(&o);
    osk_overlay_pad(&o, PAD_DOWN, 0);
    osk_overlay_open(&o, NULL, NULL, 8, 0);
    osk_overlay_pad(&o, PAD_DOWN, 0);
    CHECK(o.cursor == CELL_A, "a d-pad held across the open does not move the new request");

    /* The L shoulder switches letter case. */
    osk_overlay_pad(&o, 0, 0);
    osk_overlay_pad(&o, 0, PAD_LTRIG);
    CHECK(o.lower == 1, "L switches to lower case");
}

static void test_input_type(void) {
    OskOverlay o;
    uint16_t out[8];
    osk_overlay_reset(&o);

    /* LATIN_DIGIT: digits only. */
    osk_overlay_open(&o, NULL, NULL, 8, OSK_INPUT_LATIN_DIGIT);
    CHECK(o.cursor == 0 && highlighted_is(&o, '1'), "digit-only starts on the digit 1");
    CHECK(osk_overlay_type(&o, 'a') == 0 && osk_overlay_type(&o, '!') == 0,
          "digit-only refuses letters and symbols typed on a keyboard");
    CHECK(osk_overlay_type(&o, '5') && text_is(&o, "5"), "digit-only accepts digits");
    CHECK(osk_overlay_press(&o, OSK_KEY_CASE) == 0, "digit-only has no case switch");
    osk_overlay_press(&o, OSK_KEY_DOWN);
    CHECK(o.cursor == CELL_SPACE, "down skips the disabled rows to SPACE on the wide row");
    CHECK(enabled_count(&o) == 10 + 4, "digit-only enables ten digits and four wide keys");

    /* LATIN_LOWERCASE: lower-case letters only. */
    osk_overlay_open(&o, NULL, NULL, 16, OSK_INPUT_LATIN_LOWERCASE);
    CHECK(o.cursor == CELL_A, "lower-only starts on A");
    CHECK(osk_overlay_type(&o, 'Q') && text_is(&o, "q"),
          "lower-only forces a typed letter to lower case");
    CHECK(osk_overlay_press(&o, OSK_KEY_SELECT) && text_is(&o, "qa"),
          "lower-only types grid letters as lower case");
    CHECK(osk_overlay_type(&o, '4') == 0, "lower-only refuses digits");

    /* LATIN_UPPERCASE: upper-case letters only. */
    osk_overlay_open(&o, NULL, NULL, 16, OSK_INPUT_LATIN_UPPERCASE);
    CHECK(osk_overlay_type(&o, 'z') && text_is(&o, "Z"),
          "upper-only forces a typed letter to upper case");
    CHECK(osk_overlay_press(&o, OSK_KEY_CASE) == 0 && o.lower == 0, "upper-only has no case switch");

    /* Restricted types exclude non-ASCII; ALL and a non-Latin type do not. */
    osk_overlay_open(&o, NULL, NULL, 8, OSK_INPUT_LATIN_DIGIT);
    CHECK(osk_overlay_type(&o, 0x00E9u) == 0, "digit-only refuses a non-ASCII character");
    osk_overlay_open(&o, NULL, NULL, 8, 0);
    CHECK(osk_overlay_type(&o, 0x00E9u), "ALL accepts a non-ASCII character");
    osk_overlay_open(&o, NULL, NULL, 8, 0x00000100u); /* Japanese digits: no Latin bit */
    CHECK(osk_overlay_type(&o, 0x00E9u) && o.cursor == CELL_A,
          "a non-Latin type shows the Latin grid and accepts non-ASCII");
    CHECK(osk_overlay_answer(&o, out, 8) == -1, "the request is still open");
}

static void test_session(void) {
    wchar_t out[8];
    static const wchar_t desc[] = L"Name";
    static const wchar_t initial[] = L"old";

    sr_osk_overlay_abandon();
    CHECK(sr_osk_overlay_active() == 0, "session: idle at start");
    CHECK(sr_osk_overlay_poll(desc, initial, out, 8, 0) == SR_OSK_TEXT_PENDING,
          "session: the first poll opens a request and waits");
    CHECK(sr_osk_overlay_active(), "session: the request is open");
    CHECK(sr_osk_overlay_poll(L"other", L"zz", out, 8, 0) == SR_OSK_TEXT_PENDING,
          "session: later polls wait and ignore new arguments");

    sr_osk_overlay_key(OSK_KEY_CONFIRM);
    CHECK(sr_osk_overlay_poll(desc, initial, out, 8, 0) == 1 && wcscmp(out, L"old") == 0,
          "session: a confirmed answer is handed over once");
    CHECK(sr_osk_overlay_active() == 0, "session: the handover leaves the session idle");

    CHECK(sr_osk_overlay_poll(desc, initial, out, 8, 0) == SR_OSK_TEXT_PENDING,
          "session: the next field opens a new request");
    sr_osk_overlay_key(OSK_KEY_CANCEL);
    CHECK(sr_osk_overlay_poll(desc, initial, out, 8, 0) == 0,
          "session: a cancelled field answers 0");

    CHECK(sr_osk_overlay_poll(desc, initial, out, 8, 0) == SR_OSK_TEXT_PENDING,
          "session: a poll after a cancel opens the next request");
    sr_osk_overlay_abandon();
    CHECK(sr_osk_overlay_active() == 0, "session: an abandoned request closes");
    CHECK(sr_osk_overlay_poll(desc, initial, out, 8, 0) == SR_OSK_TEXT_PENDING,
          "session: a request after an abandon opens");
    sr_osk_overlay_abandon();

    /* Digit-only typing through the session: a letter from the keyboard is refused. */
    CHECK(sr_osk_overlay_poll(desc, L"", out, 8, OSK_INPUT_LATIN_DIGIT) == SR_OSK_TEXT_PENDING,
          "session: a digit-only request opens");
    sr_osk_overlay_type('7');
    sr_osk_overlay_type('x');
    CHECK(sr_osk_overlay_view()->len == 1 && sr_osk_overlay_view()->text[0] == '7',
          "session: digit-only typing keeps the letter out");
    sr_osk_overlay_abandon();

    /* A Start still held after an answer does not answer the next field. */
    sr_osk_overlay_pad(0, 0);
    CHECK(sr_osk_overlay_poll(desc, initial, out, 8, 0) == SR_OSK_TEXT_PENDING,
          "session: a request opens for the Start test");
    sr_osk_overlay_pad(PAD_START, 0);
    CHECK(sr_osk_overlay_poll(desc, initial, out, 8, 0) == 1, "session: Start answers the field");
    CHECK(sr_osk_overlay_poll(desc, initial, out, 8, 0) == SR_OSK_TEXT_PENDING,
          "session: the next field opens");
    sr_osk_overlay_pad(PAD_START, 0);
    CHECK(sr_osk_overlay_active(), "session: a Start still held does not answer the next field");
    sr_osk_overlay_abandon();

    CHECK(sr_osk_overlay_poll(desc, initial, NULL, 8, 0) == 0, "session: a null out is refused");
    sr_osk_overlay_set_host(1);
    CHECK(sr_osk_overlay_host() == 1, "session: the presenter host flag is set");
    sr_osk_overlay_set_host(0);
    CHECK(sr_osk_overlay_host() == 0, "session: the presenter host flag clears");
}

int main(void) {
    test_open_and_cells();
    test_navigation();
    test_typing_and_case();
    test_length_limit_and_unicode();
    test_cancel_confirm_and_answer();
    test_pad_edges();
    test_input_type();
    test_session();
    printf("osk-overlay-selftest: %d checks, %d failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
