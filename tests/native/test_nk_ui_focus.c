/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Focus graph, input model, prompt glyph and SDL translation tests for nk_ui.

   The default run is SDL-free and needs no Clay, font or GPU. The SDL translation
   leg runs with --sdl-input: it exits 77 (SKIP) with a named reason when the test
   was built without SDL3, and runs the translation checks when built with
   -DNK_UI_WITH_SDL3. Pass --print to dump the prompt glyph goldens. */

#include "nk_ui.h"
#include "nk_ui_assets.h"
#include "nk_ui_draw.h"
#include "nk_ui_focus.h"
#include "nk_ui_input.h"
#include "nk_ui_theme.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef NK_UI_WITH_SDL3
#include <SDL3/SDL.h>
#endif

size_t nk_ui_dump_draw_list(const NkUiDrawList *list, char *out, size_t cap);

#define CHECK(condition, message) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL (line %d): %s\n", __LINE__, message); \
        return 0; \
    } \
} while (0)

/* ------------------------------------------------------------------ */
/* Focus graph                                                          */
/* ------------------------------------------------------------------ */

static void reg(NkUiFocusGraph *g, NkUiId id, float x, float y, float w, float h,
                uint32_t flags, uint32_t scope, NkUiId scroller) {
    NkUiFocusEntry e;
    e.id = id;
    e.rect.x = x;
    e.rect.y = y;
    e.rect.w = w;
    e.rect.h = h;
    e.flags = flags;
    e.scope = scope;
    e.scroller = scroller;
    if (!nk_ui_focus_register(g, &e)) {
        fprintf(stderr, "register failed for id %u\n", (unsigned)id);
        exit(2);
    }
}

/* ids 1..count in rows of `cols`; cells are 100x100 with a 10 pixel gap. */
static void grid(NkUiFocusGraph *g, int cols, int count, uint32_t flags, uint32_t scope) {
    int i;
    for (i = 0; i < count; ++i) {
        reg(g, (NkUiId)(i + 1), (float)((i % cols) * 110), (float)((i / cols) * 110),
            100.0f, 100.0f, flags, scope, 0u);
    }
}

static int test_grid(void) {
    NkUiFocusGraph *g = nk_ui_focus_graph_create();
    CHECK(g != NULL, "create graph");
    nk_ui_focus_begin_frame(g);
    grid(g, 3, 9, 0u, NK_UI_SCOPE_ROOT);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 1u, "focus starts on the first entry");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_RIGHT) == 2u, "right from 1 reaches 2");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_RIGHT) == 3u, "right from 2 reaches 3");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_RIGHT) == 0u, "right edge does not wrap");
    CHECK(nk_ui_focus_current(g) == 3u, "a failed move keeps focus");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 6u, "down from 3 reaches 6");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 9u, "down from 6 reaches 9");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 0u, "bottom edge does not wrap");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_LEFT) == 8u, "left from 9 reaches 8");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_UP) == 5u, "up from 8 reaches 5");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_UP) == 2u, "up from 5 reaches 2 (not a diagonal)");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_UP) == 0u, "top edge does not wrap");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_NONE) == 0u, "no direction no move");
    CHECK(nk_ui_focus_directional(NULL, 0, 5u, NK_UI_DIR_UP) == 5u, "stateless pick on NULL");
    nk_ui_focus_graph_destroy(g);
    return 1;
}

static int test_ragged_row(void) {
    NkUiFocusGraph *g = nk_ui_focus_graph_create();
    NkUiFocusEntry list[3];
    CHECK(g != NULL, "create graph");
    /* Seven tiles in rows of three: the last row holds one tile under column 0. */
    nk_ui_focus_begin_frame(g);
    grid(g, 3, 7, 0u, NK_UI_SCOPE_ROOT);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_set(g, 6u), "focus tile 6 (row 1, column 2)");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 7u, "down from the last column lands on the ragged tile");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_UP) == 4u, "up from the ragged tile returns to column 0");
    CHECK(nk_ui_focus_set(g, 5u), "focus tile 5");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 7u, "down from the middle column lands on the ragged tile");

    /* Eight tiles: the last row has two. Column 2 chooses the nearer one. */
    nk_ui_focus_begin_frame(g);
    grid(g, 3, 8, 0u, NK_UI_SCOPE_ROOT);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_set(g, 6u), "focus tile 6");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 8u, "down picks the nearer tile of a short row");

    /* Tiles that are ROW_MEMBER never leave the row sideways. */
    nk_ui_focus_begin_frame(g);
    grid(g, 3, 7, NK_UI_FOCUS_ROW_MEMBER, NK_UI_SCOPE_ROOT);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_set(g, 7u), "focus the ragged tile");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_RIGHT) == 0u, "row end: right fails instead of jumping up a row");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_LEFT) == 0u, "row end: left fails");
    CHECK(nk_ui_focus_current(g) == 7u, "focus stays");

    /* Staggered rows with no overlap still connect when nothing better exists. */
    list[0].id = 1u; list[0].rect = (NkUiRect){0.0f, 0.0f, 100.0f, 30.0f};
    list[1].id = 2u; list[1].rect = (NkUiRect){150.0f, 40.0f, 100.0f, 30.0f};
    list[2].id = 3u; list[2].rect = (NkUiRect){150.0f, 0.0f, 100.0f, 30.0f};
    list[0].flags = list[1].flags = list[2].flags = 0u;
    list[0].scope = list[1].scope = list[2].scope = 0u;
    list[0].scroller = list[1].scroller = list[2].scroller = 0u;
    CHECK(nk_ui_focus_directional(list, 3, 1u, NK_UI_DIR_RIGHT) == 3u,
          "an overlapping row beats a staggered one");
    CHECK(nk_ui_focus_directional(list, 2, 1u, NK_UI_DIR_RIGHT) == 2u,
          "a staggered tile is reachable when it is the only candidate");
    nk_ui_focus_graph_destroy(g);
    return 1;
}

/* Ten 300x100 rows in a 300x200 scroller with 1000 of content, scrolled by scroll_y. */
static void list_frame(NkUiFocusGraph *g, float scroll_y) {
    NkUiFocusScroller s;
    int i;
    nk_ui_focus_begin_frame(g);
    s.id = 100u;
    s.viewport = (NkUiRect){0.0f, 0.0f, 300.0f, 200.0f};
    s.scroll_x = 0.0f;
    s.scroll_y = scroll_y;
    s.content_w = 300.0f;
    s.content_h = 1000.0f;
    if (!nk_ui_focus_register_scroller(g, &s)) {
        exit(2);
    }
    for (i = 0; i < 10; ++i) {
        reg(g, (NkUiId)(i + 1), 0.0f, (float)(i * 100) - scroll_y, 300.0f, 100.0f, 0u,
            NK_UI_SCOPE_ROOT, 100u);
    }
    nk_ui_focus_end_frame(g);
}

static int test_scroll_into_view(void) {
    NkUiFocusGraph *g = nk_ui_focus_graph_create();
    NkUiFocusScrollRequest r;
    CHECK(g != NULL, "create graph");
    list_frame(g, 0.0f);
    CHECK(nk_ui_focus_current(g) == 1u, "list starts on the first row");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 2u, "down to row 2");
    CHECK(!nk_ui_focus_take_scroll(g, &r), "a row that fits needs no scroll");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 3u, "down to row 3 (below the viewport)");
    CHECK(nk_ui_focus_take_scroll(g, &r), "row 3 asks for a scroll");
    CHECK(r.scroller == 100u && r.scroll_y == 100.0f && r.scroll_x == 0.0f,
          "minimal scroll brings row 3's bottom to the viewport bottom");
    CHECK(!nk_ui_focus_take_scroll(g, &r), "a request is taken once");

    list_frame(g, 100.0f);
    CHECK(nk_ui_focus_current(g) == 3u, "focus survives a scrolled frame");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 4u, "down to row 4");
    CHECK(nk_ui_focus_take_scroll(g, &r) && r.scroll_y == 200.0f, "scrolls one more row");

    CHECK(nk_ui_focus_scroll_into_view(g, 10u, &r), "row 10 is out of view");
    CHECK(r.scroll_y == 800.0f, "row 10 scroll lands on the content end (content 1000 less viewport 200)");

    list_frame(g, 800.0f);
    CHECK(nk_ui_focus_set(g, 10u), "focus row 10");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_UP) == 9u, "up to row 9");
    CHECK(!nk_ui_focus_take_scroll(g, &r), "row 9 is visible");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_UP) == 8u, "up to row 8");
    CHECK(nk_ui_focus_take_scroll(g, &r) && r.scroll_y == 700.0f, "scrolls up by the hidden part");

    list_frame(g, 0.0f);
    CHECK(nk_ui_focus_scroller_at(g, 10.0f, 10.0f) == 100u, "wheel target is the scroller under the pointer");
    CHECK(nk_ui_focus_scroller_at(g, 400.0f, 10.0f) == 0u, "no scroller outside it");
    CHECK(nk_ui_focus_hit_test(g, 10.0f, 150.0f) == 2u, "pointer hit-test finds row 2");
    CHECK(nk_ui_focus_current(g) != 2u, "hover does not move focus");
    nk_ui_focus_graph_destroy(g);
    return 1;
}

static int test_dialog_trap_and_scope_memory(void) {
    NkUiFocusGraph *g = nk_ui_focus_graph_create();
    int i;
    int steps;
    NkUiNavDir dirs[4] = {NK_UI_DIR_UP, NK_UI_DIR_DOWN, NK_UI_DIR_LEFT, NK_UI_DIR_RIGHT};
    CHECK(g != NULL, "create graph");
    nk_ui_focus_begin_frame(g);
    grid(g, 2, 4, 0u, NK_UI_SCOPE_ROOT);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_RIGHT) == 2u, "root: right");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 4u, "root: down to the last tile");
    CHECK(nk_ui_focus_active_scope(g) == NK_UI_SCOPE_ROOT, "root is the active scope");

    /* A dialog opens over the same screen area. */
    nk_ui_focus_begin_frame(g);
    grid(g, 2, 4, 0u, NK_UI_SCOPE_ROOT);
    reg(g, 10u, 20.0f, 20.0f, 100.0f, 40.0f, 0u, NK_UI_SCOPE_DIALOG, 0u);
    reg(g, 11u, 130.0f, 20.0f, 100.0f, 40.0f, NK_UI_FOCUS_DEFAULT, NK_UI_SCOPE_DIALOG, 0u);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_active_scope(g) == NK_UI_SCOPE_DIALOG, "the dialog is the top scope");
    CHECK(nk_ui_focus_current(g) == 11u, "focus enters the dialog on its DEFAULT entry");
    CHECK(nk_ui_focus_remembered(g, NK_UI_SCOPE_ROOT) == 4u, "root remembers where it was");
    CHECK(!nk_ui_focus_set(g, 1u), "a trapped scope refuses focus on a root entry");
    for (steps = 0; steps < 12; ++steps) {
        for (i = 0; i < 4; ++i) {
            (void)nk_ui_focus_move(g, dirs[i]);
            CHECK(nk_ui_focus_current(g) == 10u || nk_ui_focus_current(g) == 11u,
                  "directional moves never leave the dialog");
        }
        (void)nk_ui_focus_cycle(g, (steps & 1) != 0);
        CHECK(nk_ui_focus_current(g) == 10u || nk_ui_focus_current(g) == 11u,
              "Tab never leaves the dialog");
    }
    CHECK(nk_ui_focus_hit_test(g, 10.0f, 150.0f) == 0u, "root entries do not take pointer hits under a dialog");
    CHECK(nk_ui_focus_hit_test(g, 30.0f, 30.0f) == 10u, "dialog entries do");

    /* Back closes the dialog: focus returns to where it came from. */
    nk_ui_focus_begin_frame(g);
    grid(g, 2, 4, 0u, NK_UI_SCOPE_ROOT);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 4u, "closing the dialog restores the root focus");
    CHECK(nk_ui_focus_remembered(g, NK_UI_SCOPE_DIALOG) == 0u, "a closed scope forgets its focus");

    /* Reopened, it starts fresh on its DEFAULT entry. */
    nk_ui_focus_begin_frame(g);
    grid(g, 2, 4, 0u, NK_UI_SCOPE_ROOT);
    reg(g, 10u, 20.0f, 20.0f, 100.0f, 40.0f, 0u, NK_UI_SCOPE_DIALOG, 0u);
    reg(g, 11u, 130.0f, 20.0f, 100.0f, 40.0f, NK_UI_FOCUS_DEFAULT, NK_UI_SCOPE_DIALOG, 0u);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 11u, "a reopened dialog starts on its DEFAULT entry");

    /* A stack: root, then overlay, then drawer. Each returns to its own memory. */
    nk_ui_focus_begin_frame(g);
    grid(g, 2, 4, 0u, NK_UI_SCOPE_ROOT);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 4u, "back on root");
    nk_ui_focus_begin_frame(g);
    grid(g, 2, 4, 0u, NK_UI_SCOPE_ROOT);
    reg(g, 20u, 300.0f, 0.0f, 80.0f, 40.0f, 0u, NK_UI_SCOPE_OVERLAY, 0u);
    reg(g, 21u, 300.0f, 50.0f, 80.0f, 40.0f, 0u, NK_UI_SCOPE_OVERLAY, 0u);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 20u, "overlay opens on its first entry");
    CHECK(nk_ui_focus_move(g, NK_UI_DIR_DOWN) == 21u, "overlay: down");
    nk_ui_focus_begin_frame(g);
    grid(g, 2, 4, 0u, NK_UI_SCOPE_ROOT);
    reg(g, 20u, 300.0f, 0.0f, 80.0f, 40.0f, 0u, NK_UI_SCOPE_OVERLAY, 0u);
    reg(g, 21u, 300.0f, 50.0f, 80.0f, 40.0f, 0u, NK_UI_SCOPE_OVERLAY, 0u);
    reg(g, 30u, 400.0f, 0.0f, 80.0f, 40.0f, 0u, NK_UI_SCOPE_DRAWER, 0u);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 30u, "drawer opens above the overlay");
    nk_ui_focus_begin_frame(g);
    grid(g, 2, 4, 0u, NK_UI_SCOPE_ROOT);
    reg(g, 20u, 300.0f, 0.0f, 80.0f, 40.0f, 0u, NK_UI_SCOPE_OVERLAY, 0u);
    reg(g, 21u, 300.0f, 50.0f, 80.0f, 40.0f, 0u, NK_UI_SCOPE_OVERLAY, 0u);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 21u, "closing the drawer returns to the overlay's entry");
    nk_ui_focus_begin_frame(g);
    grid(g, 2, 4, 0u, NK_UI_SCOPE_ROOT);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 4u, "closing the overlay returns to the root's entry");
    nk_ui_focus_graph_destroy(g);
    return 1;
}

static int test_tab_cycle_and_repair(void) {
    NkUiFocusGraph *g = nk_ui_focus_graph_create();
    NkUiFocusEntry bad;
    CHECK(g != NULL, "create graph");
    /* Declaration order differs from screen order on purpose. */
    nk_ui_focus_begin_frame(g);
    reg(g, 5u, 300.0f, 300.0f, 50.0f, 50.0f, 0u, NK_UI_SCOPE_ROOT, 0u);
    reg(g, 1u, 0.0f, 0.0f, 50.0f, 50.0f, 0u, NK_UI_SCOPE_ROOT, 0u);
    reg(g, 7u, 100.0f, 0.0f, 50.0f, 50.0f, NK_UI_FOCUS_DISABLED, NK_UI_SCOPE_ROOT, 0u);
    reg(g, 9u, 0.0f, 100.0f, 50.0f, 50.0f, 0u, NK_UI_SCOPE_ROOT, 0u);
    reg(g, 3u, 100.0f, 100.0f, 50.0f, 50.0f, 0u, NK_UI_SCOPE_ROOT, 0u);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 5u, "focus starts on the first declared entry");
    CHECK(nk_ui_focus_cycle(g, false) == 1u, "Tab: 5 -> 1");
    CHECK(nk_ui_focus_cycle(g, false) == 9u, "Tab skips the disabled 7");
    CHECK(nk_ui_focus_cycle(g, false) == 3u, "Tab: 9 -> 3");
    CHECK(nk_ui_focus_cycle(g, false) == 5u, "Tab wraps");
    CHECK(nk_ui_focus_cycle(g, true) == 3u, "Shift+Tab wraps backwards");
    CHECK(nk_ui_focus_cycle(g, true) == 9u, "Shift+Tab: 3 -> 9");
    CHECK(!nk_ui_focus_set(g, 7u), "a disabled entry cannot be focused");

    /* The focused entry disappears: focus falls back to a live one. */
    nk_ui_focus_begin_frame(g);
    reg(g, 5u, 300.0f, 300.0f, 50.0f, 50.0f, 0u, NK_UI_SCOPE_ROOT, 0u);
    reg(g, 1u, 0.0f, 0.0f, 50.0f, 50.0f, 0u, NK_UI_SCOPE_ROOT, 0u);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 5u, "a vanished focus falls back to the first entry");

    /* The focused entry becomes disabled. */
    nk_ui_focus_begin_frame(g);
    reg(g, 5u, 300.0f, 300.0f, 50.0f, 50.0f, NK_UI_FOCUS_DISABLED, NK_UI_SCOPE_ROOT, 0u);
    reg(g, 1u, 0.0f, 0.0f, 50.0f, 50.0f, 0u, NK_UI_SCOPE_ROOT, 0u);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 1u, "a disabled focus moves to an enabled entry");
    CHECK(nk_ui_focus_cycle(g, false) == 0u, "Tab with one enabled entry changes nothing");

    /* Empty frame: nothing to focus. */
    nk_ui_focus_begin_frame(g);
    nk_ui_focus_end_frame(g);
    CHECK(nk_ui_focus_current(g) == 0u && nk_ui_focus_active_scope(g) == -1,
          "an empty frame clears focus");

    /* Registration validation. */
    nk_ui_focus_begin_frame(g);
    memset(&bad, 0, sizeof(bad));
    CHECK(!nk_ui_focus_register(g, &bad), "id 0 is rejected");
    bad.id = 1u; bad.rect.w = -1.0f;
    CHECK(!nk_ui_focus_register(g, &bad), "negative size is rejected");
    bad.rect.w = NAN;
    CHECK(!nk_ui_focus_register(g, &bad), "NaN size is rejected");
    bad.rect.w = 10.0f; bad.scope = NK_UI_FOCUS_MAX_SCOPES;
    CHECK(!nk_ui_focus_register(g, &bad), "scope out of range is rejected");
    bad.scope = 0u;
    CHECK(nk_ui_focus_register(g, &bad), "a valid entry registers");
    CHECK(!nk_ui_focus_register(g, &bad), "a duplicate id is rejected");
    CHECK(!nk_ui_focus_register(NULL, &bad) && !nk_ui_focus_register(g, NULL), "NULL arguments are rejected");
    nk_ui_focus_graph_destroy(g);
    return 1;
}

/* ------------------------------------------------------------------ */
/* Input model                                                          */
/* ------------------------------------------------------------------ */

static int test_repeat_timing(void) {
    NkUiInput in;
    NkUiEvent ev[NK_UI_INPUT_MAX_EVENTS];
    NkUiEvent poll[16];
    NkUiRepeatConfig cfg = nk_ui_repeat_config_default();
    double wake = 0.0;
    size_t n;
    int rate;
    static const struct { NkUiRepeatRate rate; double interval; } rates[] = {
        {NK_UI_REPEAT_SLOW, 0.110}, {NK_UI_REPEAT_NORMAL, 0.070}, {NK_UI_REPEAT_FAST, 0.045}
    };
    CHECK(cfg.initial_delay_ms == 400u && cfg.slow_ms == 110u && cfg.normal_ms == 70u &&
          cfg.fast_ms == 45u, "default repeat timings");
    CHECK(nk_ui_repeat_interval_ms(&cfg, NK_UI_REPEAT_SLOW) == 110u &&
          nk_ui_repeat_interval_ms(&cfg, NK_UI_REPEAT_NORMAL) == 70u &&
          nk_ui_repeat_interval_ms(&cfg, NK_UI_REPEAT_FAST) == 45u &&
          nk_ui_repeat_interval_ms(NULL, NK_UI_REPEAT_NORMAL) == 70u, "interval per rate");

    for (rate = 0; rate < 3; ++rate) {
        double t0 = 10.0;
        double interval = rates[rate].interval;
        int k;
        nk_ui_input_init(&in);
        nk_ui_input_set_repeat_rate(&in, rates[rate].rate);
        CHECK(!nk_ui_input_next_wake(&in, &wake), "nothing held, nothing to wake for");
        n = nk_ui_input_button(&in, NK_UI_BTN_RIGHT, true, 7, NK_UI_PAD_NEUTRAL, t0, ev, 4);
        CHECK(n == 2 && ev[0].type == NK_UI_EV_DEVICE && ev[1].type == NK_UI_EV_NAV &&
              ev[1].dir == NK_UI_DIR_RIGHT && ev[1].down && !ev[1].repeat,
              "the first press is immediate (after the device change)");
        CHECK(nk_ui_input_next_wake(&in, &wake) && fabs(wake - (t0 + 0.4)) < 1e-9,
              "the first repeat waits 400 ms");
        CHECK(nk_ui_input_poll(&in, t0 + 0.399, poll, 16) == 0u, "no repeat before the delay");
        n = nk_ui_input_poll(&in, t0 + 0.400, poll, 16);
        CHECK(n == 1 && poll[0].type == NK_UI_EV_NAV && poll[0].repeat && poll[0].down &&
              poll[0].dir == NK_UI_DIR_RIGHT && poll[0].device == 7, "first repeat at 400 ms");
        CHECK(nk_ui_input_poll(&in, t0 + 0.400 + interval - 0.001, poll, 16) == 0u,
              "nothing between repeats");
        for (k = 2; k <= 5; ++k) {
            n = nk_ui_input_poll(&in, t0 + 0.400 + interval * (double)(k - 1) + 1e-9, poll, 16);
            CHECK(n == 1, "one repeat per interval");
        }
        /* A frame that arrives two intervals late delivers both repeats. */
        n = nk_ui_input_poll(&in, t0 + 0.400 + interval * 6.0 + 1e-6, poll, 16);
        CHECK(n == 2, "a late frame catches up the repeats it owes");
        n = nk_ui_input_button(&in, NK_UI_BTN_RIGHT, false, 7, NK_UI_PAD_NEUTRAL, t0 + 2.0, ev, 4);
        CHECK(n == 0u, "a direction release reports nothing");
        CHECK(!nk_ui_input_next_wake(&in, &wake), "release stops the repeat");
        CHECK(nk_ui_input_poll(&in, t0 + 9.0, poll, 16) == 0u, "no repeat after release");
    }

    /* A long stall is coalesced rather than replayed. */
    nk_ui_input_init(&in);
    (void)nk_ui_input_button(&in, NK_UI_BTN_DOWN, true, 0, NK_UI_PAD_KEYBOARD, 0.0, ev, 4);
    n = nk_ui_input_poll(&in, 60.0, poll, 16);
    CHECK(n > 0u && n <= 8u, "a stall delivers a bounded burst");
    CHECK(nk_ui_input_next_wake(&in, &wake) && wake > 60.0, "and the schedule jumps ahead");
    CHECK(nk_ui_input_poll(&in, 60.0, poll, 16) == 0u, "nothing is owed right after");

    /* Two directions: the latest repeats; letting it go resumes the other after a new delay. */
    nk_ui_input_init(&in);
    (void)nk_ui_input_button(&in, NK_UI_BTN_RIGHT, true, 0, NK_UI_PAD_KEYBOARD, 0.0, ev, 4);
    (void)nk_ui_input_button(&in, NK_UI_BTN_DOWN, true, 0, NK_UI_PAD_KEYBOARD, 0.1, ev, 4);
    n = nk_ui_input_poll(&in, 0.5, poll, 16);
    CHECK(n == 1 && poll[0].dir == NK_UI_DIR_DOWN, "the newest held direction repeats");
    (void)nk_ui_input_button(&in, NK_UI_BTN_DOWN, false, 0, NK_UI_PAD_KEYBOARD, 0.6, ev, 4);
    CHECK(nk_ui_input_poll(&in, 0.7, poll, 16) == 0u, "the older direction restarts its delay");
    n = nk_ui_input_poll(&in, 1.0, poll, 16);
    CHECK(n == 1 && poll[0].dir == NK_UI_DIR_RIGHT, "and then repeats");
    nk_ui_input_reset(&in);
    CHECK(!nk_ui_input_next_wake(&in, &wake), "reset drops every repeat");
    return 1;
}

static int test_stick_hysteresis(void) {
    NkUiInput in;
    NkUiEvent ev[NK_UI_INPUT_MAX_EVENTS];
    size_t n;
    nk_ui_input_init(&in);
    CHECK(nk_ui_input_stick(&in, 0.49f, 0.0f, 3, NK_UI_PAD_NEUTRAL, 0.0, ev, 4) == 0u,
          "below 0.5 nothing engages");
    CHECK(!in.have_device, "drift below the threshold is not device activity");
    n = nk_ui_input_stick(&in, 0.50f, 0.0f, 3, NK_UI_PAD_NEUTRAL, 0.0, ev, 4);
    CHECK(n == 2 && ev[0].type == NK_UI_EV_DEVICE && ev[1].type == NK_UI_EV_NAV &&
          ev[1].dir == NK_UI_DIR_RIGHT, "0.5 engages right");
    CHECK(nk_ui_input_stick(&in, 0.40f, 0.0f, 3, NK_UI_PAD_NEUTRAL, 0.1, ev, 4) == 0u,
          "between 0.35 and 0.5 the direction holds");
    CHECK(nk_ui_input_stick(&in, 1.0f, 0.0f, 3, NK_UI_PAD_NEUTRAL, 0.1, ev, 4) == 0u,
          "a deeper push does not re-fire");
    CHECK(nk_ui_input_stick(&in, 0.34f, 0.0f, 3, NK_UI_PAD_NEUTRAL, 0.2, ev, 4) == 0u,
          "below 0.35 it releases (no event)");
    CHECK(in.stick_dir == NK_UI_DIR_NONE, "stick state is neutral");
    CHECK(nk_ui_input_stick(&in, 0.40f, 0.0f, 3, NK_UI_PAD_NEUTRAL, 0.3, ev, 4) == 0u,
          "0.40 does not re-engage after a release");
    n = nk_ui_input_stick(&in, 0.0f, 0.9f, 3, NK_UI_PAD_NEUTRAL, 0.4, ev, 4);
    CHECK(n == 1 && ev[0].dir == NK_UI_DIR_DOWN, "y positive is down (same device, no new device event)");
    n = nk_ui_input_stick(&in, 0.0f, -0.2f, 3, NK_UI_PAD_NEUTRAL, 0.5, ev, 4);
    CHECK(n == 0u, "released");
    n = nk_ui_input_stick(&in, -0.8f, 0.7f, 3, NK_UI_PAD_NEUTRAL, 0.6, ev, 4);
    CHECK(n == 1 && ev[0].dir == NK_UI_DIR_LEFT, "the stronger axis wins a diagonal");
    n = nk_ui_input_stick(&in, 0.0f, 0.9f, 3, NK_UI_PAD_NEUTRAL, 0.7, ev, 4);
    CHECK(n == 1 && ev[0].dir == NK_UI_DIR_DOWN, "swinging to another axis switches at once");
    CHECK(nk_ui_input_stick(&in, NAN, 0.0f, 3, NK_UI_PAD_NEUTRAL, 0.8, ev, 4) == 0u,
          "non-finite input is ignored");
    return 1;
}

/* One glyph set from the active device: keyboard and mouse until a pad connects
   or is used, the pad's set while it is the active device, and back to the
   keyboard when it disconnects. Pointer motion alone never switches. */
static int test_prompt_style_follows_device(void) {
    NkUiInput in;
    NkUiEvent ev[NK_UI_INPUT_MAX_EVENTS];
    nk_ui_input_init(&in);
    CHECK(nk_ui_input_prompt_style(&in) == NK_UI_PROMPT_KEYCAPS && !nk_ui_input_pad_connected(&in),
          "no pad: keyboard and mouse glyphs");
    (void)nk_ui_input_device_change(&in, 7, NK_UI_PAD_LETTERS_A_SOUTH, true, ev, 4);
    CHECK(nk_ui_input_prompt_style(&in) == NK_UI_PROMPT_LETTERS_A_SOUTH && nk_ui_input_pad_connected(&in),
          "a pad that connects switches the glyphs at once");
    (void)nk_ui_input_pointer(&in, 10.0f, 10.0f, false, ev, 4);
    (void)nk_ui_input_pointer(&in, 20.0f, 30.0f, false, ev, 4);
    CHECK(nk_ui_input_prompt_style(&in) == NK_UI_PROMPT_LETTERS_A_SOUTH,
          "pointer motion alone keeps the controller glyphs");
    (void)nk_ui_input_button(&in, NK_UI_BTN_CONFIRM, true, NK_UI_DEVICE_KEYBOARD_MOUSE,
                             NK_UI_PAD_KEYBOARD, 1.0, ev, 4);
    (void)nk_ui_input_button(&in, NK_UI_BTN_CONFIRM, false, NK_UI_DEVICE_KEYBOARD_MOUSE,
                             NK_UI_PAD_KEYBOARD, 1.1, ev, 4);
    CHECK(nk_ui_input_prompt_style(&in) == NK_UI_PROMPT_KEYCAPS,
          "a key press makes the keyboard the active device");
    (void)nk_ui_input_button(&in, NK_UI_BTN_BACK, true, 7, NK_UI_PAD_LETTERS_A_SOUTH, 2.0, ev, 4);
    CHECK(nk_ui_input_prompt_style(&in) == NK_UI_PROMPT_LETTERS_A_SOUTH,
          "a pad button switches back to the pad's glyphs");
    (void)nk_ui_input_pointer(&in, 20.0f, 30.0f, true, ev, 4);
    CHECK(nk_ui_input_prompt_style(&in) == NK_UI_PROMPT_KEYCAPS, "a click is mouse activity");
    (void)nk_ui_input_device_change(&in, 9, NK_UI_PAD_NEUTRAL, true, ev, 4);
    CHECK(nk_ui_input_prompt_style(&in) == NK_UI_PROMPT_NEUTRAL, "a second pad takes over when it connects");
    (void)nk_ui_input_device_change(&in, 9, NK_UI_PAD_NEUTRAL, false, ev, 4);
    CHECK(nk_ui_input_prompt_style(&in) == NK_UI_PROMPT_LETTERS_A_SOUTH,
          "when the active pad leaves, the pad still connected is used");
    (void)nk_ui_input_device_change(&in, 7, NK_UI_PAD_LETTERS_A_SOUTH, false, ev, 4);
    CHECK(nk_ui_input_prompt_style(&in) == NK_UI_PROMPT_KEYCAPS && !nk_ui_input_pad_connected(&in),
          "with every pad gone the keyboard and mouse glyphs return");
    return 1;
}

static int test_event_construction(void) {
    NkUiInput in;
    NkUiEvent ev[NK_UI_INPUT_MAX_EVENTS];
    size_t n;
    nk_ui_input_init(&in);
    CHECK(nk_ui_input_last_family(&in) == NK_UI_PAD_NEUTRAL, "default family");
    n = nk_ui_input_button(&in, NK_UI_BTN_CONFIRM, true, 0, NK_UI_PAD_KEYBOARD, 1.0, ev, 4);
    CHECK(n == 2 && ev[0].type == NK_UI_EV_DEVICE && ev[0].pad_family == NK_UI_PAD_KEYBOARD &&
          ev[1].type == NK_UI_EV_ACTIVATE && ev[1].down && !ev[1].repeat,
          "a first press announces the device, then activates");
    CHECK(nk_ui_input_button(&in, NK_UI_BTN_CONFIRM, true, 0, NK_UI_PAD_KEYBOARD, 1.1, ev, 4) == 0u,
          "an OS repeat of a held button is ignored");
    n = nk_ui_input_button(&in, NK_UI_BTN_CONFIRM, false, 0, NK_UI_PAD_KEYBOARD, 1.2, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_ACTIVATE && !ev[0].down, "Confirm release is reported");
    CHECK(nk_ui_input_button(&in, NK_UI_BTN_CONFIRM, false, 0, NK_UI_PAD_KEYBOARD, 1.3, ev, 4) == 0u,
          "a release without a press is ignored");
    n = nk_ui_input_button(&in, NK_UI_BTN_BACK, true, 0, NK_UI_PAD_KEYBOARD, 2.0, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_BACK && ev[0].down, "Back press, same device");
    CHECK(nk_ui_input_button(&in, NK_UI_BTN_BACK, false, 0, NK_UI_PAD_KEYBOARD, 2.1, ev, 4) == 0u,
          "Back release is silent");
    n = nk_ui_input_button(&in, NK_UI_BTN_OPTIONS, true, 4, NK_UI_PAD_LETTERS_A_EAST, 3.0, ev, 4);
    CHECK(n == 2 && ev[0].type == NK_UI_EV_DEVICE && ev[0].device == 4 &&
          ev[0].pad_family == NK_UI_PAD_LETTERS_A_EAST && ev[1].type == NK_UI_EV_OPTIONS,
          "a different pad announces itself");
    CHECK(nk_ui_input_last_family(&in) == NK_UI_PAD_LETTERS_A_EAST && nk_ui_input_last_device(&in) == 4,
          "the last device drives Match input");
    n = nk_ui_input_button(&in, NK_UI_BTN_DETAILS, true, 4, NK_UI_PAD_LETTERS_A_EAST, 3.1, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_DETAILS, "Details");
    n = nk_ui_input_button(&in, NK_UI_BTN_TAB_PREV, true, 4, NK_UI_PAD_LETTERS_A_EAST, 3.2, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_TAB && ev[0].dir == NK_UI_DIR_LEFT, "Tab previous is TAB/left");
    n = nk_ui_input_button(&in, NK_UI_BTN_TAB_NEXT, true, 4, NK_UI_PAD_LETTERS_A_EAST, 3.3, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_TAB && ev[0].dir == NK_UI_DIR_RIGHT, "Tab next is TAB/right");
    n = nk_ui_input_button(&in, NK_UI_BTN_MENU, true, 4, NK_UI_PAD_LETTERS_A_EAST, 3.4, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_MENU, "Menu");
    n = nk_ui_input_button(&in, NK_UI_BTN_CYCLE_PREV, true, 4, NK_UI_PAD_LETTERS_A_EAST, 3.5, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_CYCLE && ev[0].dir == NK_UI_DIR_LEFT, "Shift+Tab is CYCLE/left");
    n = nk_ui_input_button(&in, NK_UI_BTN_CYCLE_NEXT, true, 4, NK_UI_PAD_LETTERS_A_EAST, 3.6, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_CYCLE && ev[0].dir == NK_UI_DIR_RIGHT, "Tab is CYCLE/right");
    CHECK(nk_ui_input_button(&in, (NkUiInputButton)99, true, 0, NK_UI_PAD_NEUTRAL, 0.0, ev, 4) == 0u,
          "an unknown button is ignored");
    CHECK(nk_ui_input_button(&in, NK_UI_BTN_BACK, true, 4, NK_UI_PAD_NEUTRAL, 4.0, ev, 0) == 0u,
          "a zero-capacity output writes nothing");

    /* Pointer: a click takes over Match input; hover is a position change only,
       so a nudged mouse never overrides a controller's glyphs. */
    nk_ui_input_init(&in);
    n = nk_ui_input_pointer(&in, 10.0f, 20.0f, false, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_POINTER && ev[0].x == 10.0f && ev[0].y == 20.0f &&
          !ev[0].down, "pointer motion is a position change only");
    CHECK(nk_ui_input_pointer(&in, 10.0f, 20.0f, false, ev, 4) == 0u, "an unchanged pointer is silent");
    n = nk_ui_input_pointer(&in, 10.0f, 20.0f, true, ev, 4);
    CHECK(n == 2 && ev[0].type == NK_UI_EV_DEVICE && ev[0].pad_family == NK_UI_PAD_KEYBOARD &&
          ev[1].down, "a click announces the keyboard/mouse device and is reported");
    n = nk_ui_input_wheel(&in, 10.0f, 20.0f, NK_UI_DIR_DOWN, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_WHEEL && ev[0].dir == NK_UI_DIR_DOWN &&
          ev[0].x == 10.0f && ev[0].y == 20.0f, "wheel notch at the pointer");
    CHECK(nk_ui_input_wheel(&in, 0.0f, 0.0f, NK_UI_DIR_NONE, ev, 4) == 0u, "a wheel needs a direction");
    n = nk_ui_input_text(&in, 0x3042u, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_TEXT && ev[0].codepoint == 0x3042u, "text input");
    n = nk_ui_input_device_change(&in, 9, NK_UI_PAD_LETTERS_A_SOUTH, true, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_DEVICE && ev[0].down && ev[0].device == 9, "pad connected");
    n = nk_ui_input_device_change(&in, 9, NK_UI_PAD_LETTERS_A_SOUTH, false, ev, 4);
    CHECK(n == 1 && ev[0].type == NK_UI_EV_DEVICE && !ev[0].down, "pad disconnected");
    return 1;
}

static int test_utf8(void) {
    const char text[] = "a\xC3\xA9\xE3\x81\x82\xF0\x9F\x98\x80\xFF" "z";
    const char *p = text;
    const char *end = text + sizeof(text) - 1;
    CHECK(nk_ui_utf8_next(&p, end) == 'a', "ascii");
    CHECK(nk_ui_utf8_next(&p, end) == 0xE9u, "two bytes");
    CHECK(nk_ui_utf8_next(&p, end) == 0x3042u, "three bytes");
    CHECK(nk_ui_utf8_next(&p, end) == 0x1F600u, "four bytes");
    CHECK(nk_ui_utf8_next(&p, end) == 0xFFFDu, "an invalid lead byte is replaced");
    CHECK(nk_ui_utf8_next(&p, end) == 'z', "decoding resumes after it");
    CHECK(nk_ui_utf8_next(&p, end) == 0u, "end of input");
    {
        const char truncated[] = "\xE3\x81";
        const char *q = truncated;
        CHECK(nk_ui_utf8_next(&q, truncated + 2) == 0xFFFDu && q == truncated + 1,
              "a truncated sequence is replaced and consumes one byte");
        CHECK(nk_ui_utf8_next(&q, truncated + 2) == 0xFFFDu, "so does the stray continuation");
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* Prompts                                                              */
/* ------------------------------------------------------------------ */

static int test_label_table(void) {
    int label;
    CHECK(nk_ui_pad_family_for_south_label(NK_UI_LABEL_A) == NK_UI_PAD_LETTERS_A_SOUTH,
          "A on south: letters, A south");
    CHECK(nk_ui_pad_family_for_south_label(NK_UI_LABEL_B) == NK_UI_PAD_LETTERS_A_EAST,
          "B on south: letters, A east");
    CHECK(nk_ui_pad_family_for_south_label(NK_UI_LABEL_CROSS) == NK_UI_PAD_SHAPES,
          "a cross on south: shapes");
    for (label = 0; label <= NK_UI_LABEL_COUNT; ++label) {
        if (label == NK_UI_LABEL_A || label == NK_UI_LABEL_B || label == NK_UI_LABEL_CROSS) {
            continue;
        }
        CHECK(nk_ui_pad_family_for_south_label((NkUiButtonLabel)label) == NK_UI_PAD_NEUTRAL,
              "every other label, and unknown values, get the neutral set");
    }
    CHECK(nk_ui_pad_family_for_south_label((NkUiButtonLabel)-5) == NK_UI_PAD_NEUTRAL,
          "a negative value is unknown");
    CHECK(nk_ui_prompt_style_for_family(NK_UI_PAD_NEUTRAL) == NK_UI_PROMPT_NEUTRAL &&
          nk_ui_prompt_style_for_family(NK_UI_PAD_LETTERS_A_SOUTH) == NK_UI_PROMPT_LETTERS_A_SOUTH &&
          nk_ui_prompt_style_for_family(NK_UI_PAD_LETTERS_A_EAST) == NK_UI_PROMPT_LETTERS_A_EAST &&
          nk_ui_prompt_style_for_family(NK_UI_PAD_KEYBOARD) == NK_UI_PROMPT_KEYCAPS &&
          nk_ui_prompt_style_for_family(NK_UI_PAD_SHAPES) == NK_UI_PROMPT_SHAPES &&
          nk_ui_prompt_style_for_family((NkUiPadFamily)99) == NK_UI_PROMPT_NEUTRAL,
          "family to style");
    /* Confirm is positional south in every set. */
    CHECK(strcmp(nk_ui_prompt_text(NK_UI_PROMPT_LETTERS_A_SOUTH, NK_UI_PROMPT_ACT_CONFIRM), "A") == 0 &&
          strcmp(nk_ui_prompt_text(NK_UI_PROMPT_LETTERS_A_SOUTH, NK_UI_PROMPT_ACT_BACK), "B") == 0 &&
          strcmp(nk_ui_prompt_text(NK_UI_PROMPT_LETTERS_A_SOUTH, NK_UI_PROMPT_ACT_OPTIONS), "X") == 0 &&
          strcmp(nk_ui_prompt_text(NK_UI_PROMPT_LETTERS_A_SOUTH, NK_UI_PROMPT_ACT_DETAILS), "Y") == 0,
          "letters, A south");
    CHECK(strcmp(nk_ui_prompt_text(NK_UI_PROMPT_LETTERS_A_EAST, NK_UI_PROMPT_ACT_CONFIRM), "B") == 0 &&
          strcmp(nk_ui_prompt_text(NK_UI_PROMPT_LETTERS_A_EAST, NK_UI_PROMPT_ACT_BACK), "A") == 0 &&
          strcmp(nk_ui_prompt_text(NK_UI_PROMPT_LETTERS_A_EAST, NK_UI_PROMPT_ACT_OPTIONS), "Y") == 0 &&
          strcmp(nk_ui_prompt_text(NK_UI_PROMPT_LETTERS_A_EAST, NK_UI_PROMPT_ACT_DETAILS), "X") == 0,
          "letters, A east: the printed letters swap, the positions do not");
    CHECK(nk_ui_prompt_text(NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_CONFIRM) == NULL &&
          nk_ui_prompt_text(NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_MOVE) == NULL,
          "the neutral set is pure geometry");
    CHECK(strcmp(nk_ui_prompt_text(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_CONFIRM), "ENTER") == 0 &&
          strcmp(nk_ui_prompt_text(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_BACK), "ESC") == 0 &&
          strcmp(nk_ui_prompt_text(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_OPTIONS), "SPACE") == 0 &&
          strcmp(nk_ui_prompt_text(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_TAB_PREV), "Q") == 0 &&
          strcmp(nk_ui_prompt_text(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_TAB_NEXT), "E") == 0,
          "keycaps");
    CHECK(nk_ui_prompt_width(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_CONFIRM, 20.0f) >
              nk_ui_prompt_width(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_TAB_NEXT, 20.0f) &&
          nk_ui_prompt_width(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_TAB_NEXT, 20.0f) == 20.0f &&
          nk_ui_prompt_width(NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_CONFIRM, 20.0f) == 20.0f &&
          nk_ui_prompt_width(NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_TAB_NEXT, 20.0f) == 32.0f &&
          nk_ui_prompt_width(NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_CONFIRM, 0.0f) == 0.0f,
          "glyph widths");
    return 1;
}

static const char *const g_style_names[NK_UI_PROMPT_STYLE_COUNT] = {
    "neutral", "letters-a-south", "letters-a-east", "keycaps", "shapes"
};
static const char *const g_action_names[NK_UI_PROMPT_ACT_COUNT] = {
    "confirm", "back", "options", "details", "tab-prev", "tab-next", "menu", "move"
};

static NkUiPromptSpec spec_for(NkUiPromptStyle style, NkUiPromptAction action) {
    const NkUiTheme *theme = nk_ui_theme_n1();
    NkUiPromptSpec s;
    s.style = style;
    s.action = action;
    s.fg_rgba = theme->paper;
    s.dim_rgba = theme->muted;
    s.face_rgba = theme->surface_high;
    s.font = 0;
    s.image = NULL;
    return s;
}

static int dump_prompt(NkUiPromptStyle style, NkUiPromptAction action, float height,
                       float density, char *out, size_t cap) {
    NkUiDrawBuffer buffer;
    NkUiPromptSpec spec = spec_for(style, action);
    NkUiRect rect;
    int ok;
    memset(&buffer, 0, sizeof(buffer));
    rect.x = 10.0f;
    rect.y = 4.0f;
    rect.h = height;
    rect.w = nk_ui_prompt_width(style, action, height);
    ok = nk_ui_prompt_draw(&buffer, &spec, rect, density, 0xC4u);
    if (ok) {
        (void)nk_ui_dump_draw_list(&buffer.list, out, cap);
    }
    nk_ui_draw_buffer_destroy(&buffer);
    return ok;
}

/* Each glyph must stay inside its box at every supported density. */
static int test_prompt_bounds(void) {
    static const float densities[4] = {1.0f, 1.25f, 1.5f, 2.0f};
    int style;
    int action;
    int d;
    for (style = 0; style < (int)NK_UI_PROMPT_STYLE_COUNT; ++style) {
        for (action = 0; action < NK_UI_PROMPT_ACT_COUNT; ++action) {
            for (d = 0; d < 4; ++d) {
                NkUiDrawBuffer buffer;
                NkUiPromptSpec spec = spec_for((NkUiPromptStyle)style, (NkUiPromptAction)action);
                NkUiRect rect;
                NkUiPixelRect box;
                uint32_t i;
                int texts = 0;
                memset(&buffer, 0, sizeof(buffer));
                rect.x = 10.0f; rect.y = 4.0f; rect.h = 24.0f;
                rect.w = nk_ui_prompt_width(spec.style, spec.action, rect.h);
                CHECK(nk_ui_prompt_draw(&buffer, &spec, rect, densities[d], 5u), "glyph draws");
                CHECK(buffer.list.ncmd > 0u, "a glyph emits geometry");
                box = nk_ui_round_rect(rect, densities[d]);
                for (i = 0; i < buffer.list.ncmd; ++i) {
                    const NkUiDrawCmd *c = &buffer.list.cmd[i];
                    CHECK(c->bounds[0] >= box.x0 && c->bounds[1] >= box.y0 &&
                          c->bounds[2] <= box.x1 && c->bounds[3] <= box.y1,
                          "glyph geometry stays inside its box");
                    CHECK(c->kind == NK_UI_DRAW_RECTANGLE || c->kind == NK_UI_DRAW_TEXT,
                          "a glyph is rectangles and text only");
                    if (c->kind == NK_UI_DRAW_TEXT) {
                        ++texts;
                    }
                }
                CHECK((nk_ui_prompt_text(spec.style, spec.action) != NULL) == (texts == 1),
                      "text appears exactly when the table says the glyph has a label");
                nk_ui_draw_buffer_destroy(&buffer);
            }
        }
    }
    {
        NkUiDrawBuffer buffer;
        NkUiPromptSpec spec = spec_for(NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_CONFIRM);
        NkUiRect rect = {0.0f, 0.0f, 20.0f, 20.0f};
        memset(&buffer, 0, sizeof(buffer));
        CHECK(!nk_ui_prompt_draw(NULL, &spec, rect, 1.0f, 1u), "NULL buffer");
        CHECK(!nk_ui_prompt_draw(&buffer, NULL, rect, 1.0f, 1u), "NULL spec");
        CHECK(!nk_ui_prompt_draw(&buffer, &spec, rect, 0.0f, 1u), "zero density");
        rect.w = 0.0f;
        CHECK(!nk_ui_prompt_draw(&buffer, &spec, rect, 1.0f, 1u), "empty rect");
        rect.w = 20.0f;
        spec.action = NK_UI_PROMPT_ACT_COUNT;
        CHECK(!nk_ui_prompt_draw(&buffer, &spec, rect, 1.0f, 1u), "unknown action");
        nk_ui_draw_buffer_destroy(&buffer);
    }
    return 1;
}

/* The Kenney glyph sets: one embedded, decodable image per action for every
   style but NEUTRAL, white (so the tint is the colour), confirm positional,
   and drawn as one tinted image inside the glyph box at every density. */
static int test_prompt_images(void) {
    static const float densities[4] = {1.0f, 1.25f, 1.5f, 2.0f};
    static NkUiPromptImages images;
    const NkUiTheme *theme = nk_ui_theme_n1();
    int style;
    int action;
    int d;
    CHECK(strcmp(nk_ui_prompt_asset_name(NK_UI_PROMPT_LETTERS_A_SOUTH, NK_UI_PROMPT_ACT_CONFIRM),
                 "input-prompts/xbox_button_a.png") == 0 &&
          strcmp(nk_ui_prompt_asset_name(NK_UI_PROMPT_LETTERS_A_EAST, NK_UI_PROMPT_ACT_CONFIRM),
                 "input-prompts/switch_button_b.png") == 0 &&
          strcmp(nk_ui_prompt_asset_name(NK_UI_PROMPT_SHAPES, NK_UI_PROMPT_ACT_BACK),
                 "input-prompts/playstation_button_circle.png") == 0 &&
          strcmp(nk_ui_prompt_asset_name(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_CONFIRM),
                 "input-prompts/kbd_enter.png") == 0 &&
          strcmp(nk_ui_prompt_asset_name(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_MOVE),
                 "input-prompts/kbd_arrows.png") == 0,
          "confirm is the south button, so A east shows B; Enter and the arrows on a keyboard");
    CHECK(nk_ui_prompt_asset_name(NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_COUNT) == NULL &&
          nk_ui_prompt_asset_name(NK_UI_PROMPT_STYLE_COUNT, NK_UI_PROMPT_ACT_CONFIRM) == NULL,
          "out-of-range names");
    CHECK(nk_ui_prompt_images_load(&images) ==
              (size_t)(NK_UI_PROMPT_STYLE_COUNT - 1) * NK_UI_PROMPT_ACT_COUNT,
          "every glyph but the neutral set decodes");
    for (style = 0; style < (int)NK_UI_PROMPT_STYLE_COUNT; ++style) {
        for (action = 0; action < NK_UI_PROMPT_ACT_COUNT; ++action) {
            const NkUiImage *image = &images.image[style][action];
            const char *name = nk_ui_prompt_asset_name((NkUiPromptStyle)style,
                                                       (NkUiPromptAction)action);
            size_t i;
            if (style == NK_UI_PROMPT_NEUTRAL) {
                CHECK(name == NULL && image->rgba_pm == NULL, "the neutral set keeps its dots");
                continue;
            }
            CHECK(name != NULL && nk_ui_embedded_asset(name) != NULL && image->rgba_pm != NULL &&
                  image->width == 64u && image->height == 64u, "glyph decoded at 64 x 64");
            for (i = 0; i < (size_t)image->width * image->height; ++i) {
                const uint8_t *p = image->rgba_pm + i * 4u;
                CHECK(p[0] == p[3] && p[1] == p[3] && p[2] == p[3],
                      "glyphs are white, so the tint alone sets their colour");
            }
            for (d = 0; d < 4; ++d) {
                NkUiDrawBuffer buffer;
                NkUiPromptSpec spec = spec_for((NkUiPromptStyle)style, (NkUiPromptAction)action);
                NkUiRect rect = {10.0f, 4.0f, 24.0f, 24.0f};
                NkUiPixelRect box;
                const NkUiDrawCmd *c;
                memset(&buffer, 0, sizeof(buffer));
                spec.image = image;
                CHECK(nk_ui_prompt_draw(&buffer, &spec, rect, densities[d], 6u), "image glyph draws");
                CHECK(buffer.list.ncmd == 1u, "an image glyph is one command");
                c = &buffer.list.cmd[0];
                box = nk_ui_round_rect(rect, densities[d]);
                CHECK(c->kind == NK_UI_DRAW_CUSTOM && c->custom_kind == NK_UI_CUSTOM_IMAGE &&
                      c->custom == image, "the glyph is the image");
                CHECK(c->bounds[0] >= box.x0 && c->bounds[1] >= box.y0 &&
                      c->bounds[2] <= box.x1 && c->bounds[3] <= box.y1 &&
                      c->bounds[2] - c->bounds[0] == box.x1 - box.x0,
                      "the square glyph fills its square box");
                CHECK(c->rgba_pm == theme->paper, "tinted with the opaque foreground colour");
                nk_ui_draw_buffer_destroy(&buffer);
            }
        }
    }
    nk_ui_prompt_images_free(&images);
    CHECK(images.image[NK_UI_PROMPT_KEYCAPS][0].rgba_pm == NULL, "free releases every glyph");
    return 1;
}

/* Golden draw lists: "<element> <kind> x0 y0 x1 y1 premultiplied-rgba "text"". */
typedef struct {
    NkUiPromptStyle style;
    NkUiPromptAction action;
    float height;
    float density;
} GlyphCase;

static const GlyphCase g_glyph_cases[] = {
    {NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_CONFIRM, 24.0f, 1.0f},
    {NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_BACK, 24.0f, 1.0f},
    {NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_OPTIONS, 24.0f, 1.0f},
    {NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_DETAILS, 24.0f, 1.0f},
    {NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_CONFIRM, 24.0f, 2.0f},
    {NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_TAB_NEXT, 24.0f, 1.0f},
    {NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_MENU, 24.0f, 1.0f},
    {NK_UI_PROMPT_NEUTRAL, NK_UI_PROMPT_ACT_MOVE, 24.0f, 1.0f},
    {NK_UI_PROMPT_LETTERS_A_SOUTH, NK_UI_PROMPT_ACT_CONFIRM, 24.0f, 1.0f},
    {NK_UI_PROMPT_LETTERS_A_SOUTH, NK_UI_PROMPT_ACT_BACK, 24.0f, 1.5f},
    {NK_UI_PROMPT_LETTERS_A_EAST, NK_UI_PROMPT_ACT_CONFIRM, 24.0f, 1.0f},
    {NK_UI_PROMPT_LETTERS_A_EAST, NK_UI_PROMPT_ACT_BACK, 24.0f, 1.5f},
    {NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_MOVE, 24.0f, 1.0f},
    {NK_UI_PROMPT_LETTERS_A_SOUTH, NK_UI_PROMPT_ACT_MOVE, 24.0f, 1.0f},
    {NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_CONFIRM, 24.0f, 1.0f},
    {NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_BACK, 24.0f, 2.0f},
    {NK_UI_PROMPT_KEYCAPS, NK_UI_PROMPT_ACT_TAB_NEXT, 24.0f, 1.0f}
};

/*GOLDEN-BEGIN*/
/* Golden draw lists for the prompt glyphs. Regenerate with --print and review the diff.
   Colors are the N1 tokens: paper F5F7FA, muted 9AA4B2, surface-high 18202B. */
static const char g_glyph_golden[] =
    "# neutral confirm h=24 density=1.00\n"
    "000000C4 RECT 10 4 34 28 9AA4B2FF \"\"\n"
    "000000C4 RECT 11 5 33 27 18202BFF \"\"\n"
    "000000C4 RECT 19 6 25 12 9AA4B2FF \"\"\n"
    "000000C4 RECT 26 13 32 19 9AA4B2FF \"\"\n"
    "000000C4 RECT 19 20 25 26 F5F7FAFF \"\"\n"
    "000000C4 RECT 12 13 18 19 9AA4B2FF \"\"\n"
    "# neutral back h=24 density=1.00\n"
    "000000C4 RECT 10 4 34 28 9AA4B2FF \"\"\n"
    "000000C4 RECT 11 5 33 27 18202BFF \"\"\n"
    "000000C4 RECT 19 6 25 12 9AA4B2FF \"\"\n"
    "000000C4 RECT 26 13 32 19 F5F7FAFF \"\"\n"
    "000000C4 RECT 19 20 25 26 9AA4B2FF \"\"\n"
    "000000C4 RECT 12 13 18 19 9AA4B2FF \"\"\n"
    "# neutral options h=24 density=1.00\n"
    "000000C4 RECT 10 4 34 28 9AA4B2FF \"\"\n"
    "000000C4 RECT 11 5 33 27 18202BFF \"\"\n"
    "000000C4 RECT 19 6 25 12 9AA4B2FF \"\"\n"
    "000000C4 RECT 26 13 32 19 9AA4B2FF \"\"\n"
    "000000C4 RECT 19 20 25 26 9AA4B2FF \"\"\n"
    "000000C4 RECT 12 13 18 19 F5F7FAFF \"\"\n"
    "# neutral details h=24 density=1.00\n"
    "000000C4 RECT 10 4 34 28 9AA4B2FF \"\"\n"
    "000000C4 RECT 11 5 33 27 18202BFF \"\"\n"
    "000000C4 RECT 19 6 25 12 F5F7FAFF \"\"\n"
    "000000C4 RECT 26 13 32 19 9AA4B2FF \"\"\n"
    "000000C4 RECT 19 20 25 26 9AA4B2FF \"\"\n"
    "000000C4 RECT 12 13 18 19 9AA4B2FF \"\"\n"
    "# neutral confirm h=24 density=2.00\n"
    "000000C4 RECT 20 8 68 56 9AA4B2FF \"\"\n"
    "000000C4 RECT 21 9 67 55 18202BFF \"\"\n"
    "000000C4 RECT 37 11 51 25 9AA4B2FF \"\"\n"
    "000000C4 RECT 51 25 65 39 9AA4B2FF \"\"\n"
    "000000C4 RECT 37 39 51 53 F5F7FAFF \"\"\n"
    "000000C4 RECT 23 25 37 39 9AA4B2FF \"\"\n"
    "# neutral tab-next h=24 density=1.00\n"
    "000000C4 RECT 10 4 48 28 18202BFF \"\"\n"
    "000000C4 TEXT 10 4 48 28 F5F7FAFF \"R\"\n"
    "# neutral menu h=24 density=1.00\n"
    "000000C4 RECT 12 6 32 26 18202BFF \"\"\n"
    "000000C4 RECT 16 11 28 13 F5F7FAFF \"\"\n"
    "000000C4 RECT 16 15 28 17 F5F7FAFF \"\"\n"
    "000000C4 RECT 16 19 28 21 F5F7FAFF \"\"\n"
    "# neutral move h=24 density=1.00\n"
    "000000C4 RECT 10 12 34 20 9AA4B2FF \"\"\n"
    "000000C4 RECT 18 4 26 28 9AA4B2FF \"\"\n"
    "000000C4 RECT 11 13 33 19 18202BFF \"\"\n"
    "000000C4 RECT 19 5 25 27 18202BFF \"\"\n"
    "000000C4 RECT 19 4 24 8 F5F7FAFF \"\"\n"
    "000000C4 RECT 29 13 33 18 F5F7FAFF \"\"\n"
    "000000C4 RECT 19 23 24 27 F5F7FAFF \"\"\n"
    "000000C4 RECT 10 13 14 18 F5F7FAFF \"\"\n"
    "# letters-a-south confirm h=24 density=1.00\n"
    "000000C4 RECT 12 6 32 26 18202BFF \"\"\n"
    "000000C4 TEXT 12 6 32 26 F5F7FAFF \"A\"\n"
    "# letters-a-south back h=24 density=1.50\n"
    "000000C4 RECT 17 8 49 40 18202BFF \"\"\n"
    "000000C4 TEXT 17 8 49 40 F5F7FAFF \"B\"\n"
    "# letters-a-east confirm h=24 density=1.00\n"
    "000000C4 RECT 12 6 32 26 18202BFF \"\"\n"
    "000000C4 TEXT 12 6 32 26 F5F7FAFF \"B\"\n"
    "# letters-a-east back h=24 density=1.50\n"
    "000000C4 RECT 17 8 49 40 18202BFF \"\"\n"
    "000000C4 TEXT 17 8 49 40 F5F7FAFF \"A\"\n"
    "# keycaps move h=24 density=1.00\n"
    "000000C4 RECT 23 5 33 15 9AA4B2FF \"\"\n"
    "000000C4 RECT 23 5 33 14 18202BFF \"\"\n"
    "000000C4 RECT 26 7 30 10 F5F7FAFF \"\"\n"
    "000000C4 RECT 11 17 21 27 9AA4B2FF \"\"\n"
    "000000C4 RECT 11 17 21 26 18202BFF \"\"\n"
    "000000C4 RECT 14 19 17 23 F5F7FAFF \"\"\n"
    "000000C4 RECT 23 17 33 27 9AA4B2FF \"\"\n"
    "000000C4 RECT 23 17 33 26 18202BFF \"\"\n"
    "000000C4 RECT 26 20 30 23 F5F7FAFF \"\"\n"
    "000000C4 RECT 35 17 45 27 9AA4B2FF \"\"\n"
    "000000C4 RECT 35 17 45 26 18202BFF \"\"\n"
    "000000C4 RECT 38 19 42 23 F5F7FAFF \"\"\n"
    "# letters-a-south move h=24 density=1.00\n"
    "000000C4 RECT 10 12 34 20 9AA4B2FF \"\"\n"
    "000000C4 RECT 18 4 26 28 9AA4B2FF \"\"\n"
    "000000C4 RECT 11 13 33 19 18202BFF \"\"\n"
    "000000C4 RECT 19 5 25 27 18202BFF \"\"\n"
    "000000C4 RECT 19 4 24 8 F5F7FAFF \"\"\n"
    "000000C4 RECT 29 13 33 18 F5F7FAFF \"\"\n"
    "000000C4 RECT 19 23 24 27 F5F7FAFF \"\"\n"
    "000000C4 RECT 10 13 14 18 F5F7FAFF \"\"\n"
    "# keycaps confirm h=24 density=1.00\n"
    "000000C4 RECT 10 4 56 28 9AA4B2FF \"\"\n"
    "000000C4 RECT 10 4 56 25 18202BFF \"\"\n"
    "000000C4 TEXT 10 4 56 25 F5F7FAFF \"ENTER\"\n"
    "# keycaps back h=24 density=2.00\n"
    "000000C4 RECT 20 8 84 56 9AA4B2FF \"\"\n"
    "000000C4 RECT 20 8 84 50 18202BFF \"\"\n"
    "000000C4 TEXT 20 8 84 50 F5F7FAFF \"ESC\"\n"
    "# keycaps tab-next h=24 density=1.00\n"
    "000000C4 RECT 10 4 34 28 9AA4B2FF \"\"\n"
    "000000C4 RECT 10 4 34 25 18202BFF \"\"\n"
    "000000C4 TEXT 10 4 34 25 F5F7FAFF \"E\"\n";
/*GOLDEN-END*/

static int build_glyph_goldens(char *out, size_t cap) {
    size_t used = 0u;
    size_t i;
    out[0] = '\0';
    for (i = 0; i < sizeof(g_glyph_cases) / sizeof(g_glyph_cases[0]); ++i) {
        const GlyphCase *c = &g_glyph_cases[i];
        char body[4096];
        int n;
        if (!dump_prompt(c->style, c->action, c->height, c->density, body, sizeof(body))) {
            return 0;
        }
        n = snprintf(out + used, cap - used, "# %s %s h=%.0f density=%.2f\n%s",
                     g_style_names[c->style], g_action_names[c->action],
                     (double)c->height, (double)c->density, body);
        if (n < 0 || (size_t)n >= cap - used) {
            return 0;
        }
        used += (size_t)n;
    }
    return 1;
}

static int test_glyph_goldens(void) {
    static char actual[65536];
    CHECK(build_glyph_goldens(actual, sizeof(actual)), "build glyph goldens");
    if (strcmp(actual, g_glyph_golden) != 0) {
        fprintf(stderr, "glyph golden mismatch; run with --print and review the diff\n");
        fputs(actual, stderr);
        return 0;
    }
    return 1;
}

/* ------------------------------------------------------------------ */
/* SDL translation                                                      */
/* ------------------------------------------------------------------ */

#ifdef NK_UI_WITH_SDL3

static NkUiEvent g_out[NK_UI_INPUT_MAX_EVENTS];

static SDL_Event key_event(SDL_Keycode key, bool down, bool repeat, SDL_Keymod mod) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.key = key;
    e.key.down = down;
    e.key.repeat = repeat;
    e.key.mod = mod;
    return e;
}

static SDL_Event pad_button(Uint8 button, bool down) {
    SDL_Event e;
    memset(&e, 0, sizeof(e));
    e.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = 41;
    e.gbutton.button = button;
    e.gbutton.down = down;
    return e;
}

static size_t tr(NkUiSdlInput *s, const SDL_Event *e, double t) {
    return nk_ui_sdl_input_translate(s, (const union SDL_Event *)e, t, g_out, NK_UI_INPUT_MAX_EVENTS);
}

static int test_sdl_translation(void) {
    NkUiSdlInput s;
    SDL_Event e;
    size_t n;
    int label;
    CHECK(nk_ui_sdl_input_available(), "built with SDL3");
    nk_ui_sdl_input_init(&s);

    /* The label table, through the real SDL enum. */
    CHECK(nk_ui_sdl_button_label(SDL_GAMEPAD_BUTTON_LABEL_A) == NK_UI_LABEL_A &&
          nk_ui_sdl_button_label(SDL_GAMEPAD_BUTTON_LABEL_B) == NK_UI_LABEL_B &&
          nk_ui_sdl_button_label(SDL_GAMEPAD_BUTTON_LABEL_X) == NK_UI_LABEL_X &&
          nk_ui_sdl_button_label(SDL_GAMEPAD_BUTTON_LABEL_Y) == NK_UI_LABEL_Y &&
          nk_ui_sdl_button_label(SDL_GAMEPAD_BUTTON_LABEL_CROSS) == NK_UI_LABEL_CROSS &&
          nk_ui_sdl_button_label(SDL_GAMEPAD_BUTTON_LABEL_CIRCLE) == NK_UI_LABEL_CIRCLE &&
          nk_ui_sdl_button_label(SDL_GAMEPAD_BUTTON_LABEL_SQUARE) == NK_UI_LABEL_SQUARE &&
          nk_ui_sdl_button_label(SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE) == NK_UI_LABEL_TRIANGLE &&
          nk_ui_sdl_button_label(SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN) == NK_UI_LABEL_UNKNOWN &&
          nk_ui_sdl_button_label(12345) == NK_UI_LABEL_UNKNOWN, "SDL label conversion");
    for (label = SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN; label <= SDL_GAMEPAD_BUTTON_LABEL_TRIANGLE; ++label) {
        NkUiPadFamily family = nk_ui_pad_family_for_south_label(nk_ui_sdl_button_label(label));
        if (label == SDL_GAMEPAD_BUTTON_LABEL_A) {
            CHECK(family == NK_UI_PAD_LETTERS_A_SOUTH, "SDL label A -> letters, A south");
        } else if (label == SDL_GAMEPAD_BUTTON_LABEL_B) {
            CHECK(family == NK_UI_PAD_LETTERS_A_EAST, "SDL label B -> letters, A east");
        } else if (label == SDL_GAMEPAD_BUTTON_LABEL_CROSS) {
            CHECK(family == NK_UI_PAD_SHAPES, "SDL label cross -> shapes");
        } else {
            CHECK(family == NK_UI_PAD_NEUTRAL, "every other SDL label -> neutral");
        }
    }

    /* Keyboard. */
    e = key_event(SDLK_RETURN, true, false, 0);
    n = tr(&s, &e, 1.0);
    CHECK(n == 2 && g_out[0].type == NK_UI_EV_DEVICE && g_out[0].pad_family == NK_UI_PAD_KEYBOARD &&
          g_out[1].type == NK_UI_EV_ACTIVATE && g_out[1].down, "Enter activates");
    e = key_event(SDLK_RETURN, true, true, 0);
    CHECK(tr(&s, &e, 1.1) == 0u, "an OS key repeat is dropped");
    e = key_event(SDLK_RETURN, false, false, 0);
    n = tr(&s, &e, 1.2);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_ACTIVATE && !g_out[0].down, "Enter release");
    e = key_event(SDLK_RETURN, true, false, SDL_KMOD_LALT);
    CHECK(tr(&s, &e, 1.3) == 0u, "Alt+Enter stays with the host (fullscreen)");
    e = key_event(SDLK_F11, true, false, 0);
    CHECK(tr(&s, &e, 1.4) == 0u, "F11 stays with the host");
    e = key_event(SDLK_ESCAPE, true, false, 0);
    n = tr(&s, &e, 1.5);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_BACK, "Esc goes back");
    e = key_event(SDLK_ESCAPE, false, false, 0);
    (void)tr(&s, &e, 1.6);
    e = key_event(SDLK_SPACE, true, false, 0);
    n = tr(&s, &e, 1.7);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_OPTIONS, "Space opens options");
    e = key_event(SDLK_SPACE, false, false, 0);
    (void)tr(&s, &e, 1.71);
    e = key_event(SDLK_Q, true, false, 0);
    n = tr(&s, &e, 1.8);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_TAB && g_out[0].dir == NK_UI_DIR_LEFT, "Q switches tab left");
    e = key_event(SDLK_Q, false, false, 0);
    (void)tr(&s, &e, 1.81);
    e = key_event(SDLK_E, true, false, 0);
    n = tr(&s, &e, 1.9);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_TAB && g_out[0].dir == NK_UI_DIR_RIGHT, "E switches tab right");
    e = key_event(SDLK_E, false, false, 0);
    (void)tr(&s, &e, 1.91);
    e = key_event(SDLK_TAB, true, false, 0);
    n = tr(&s, &e, 2.0);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_CYCLE && g_out[0].dir == NK_UI_DIR_RIGHT, "Tab cycles forward");
    e = key_event(SDLK_TAB, false, false, 0);
    (void)tr(&s, &e, 2.05);
    e = key_event(SDLK_TAB, true, false, SDL_KMOD_LSHIFT);
    n = tr(&s, &e, 2.1);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_CYCLE && g_out[0].dir == NK_UI_DIR_LEFT, "Shift+Tab cycles back");
    e = key_event(SDLK_TAB, false, false, 0);
    (void)tr(&s, &e, 2.15);
    e = key_event(SDLK_DOWN, true, false, 0);
    n = tr(&s, &e, 3.0);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_NAV && g_out[0].dir == NK_UI_DIR_DOWN && !g_out[0].repeat,
          "arrow keys navigate");
    n = nk_ui_input_poll(&s.input, 3.4, g_out, NK_UI_INPUT_MAX_EVENTS);
    CHECK(n == 1 && g_out[0].repeat && g_out[0].dir == NK_UI_DIR_DOWN, "held arrow repeats from the injected clock");
    e = key_event(SDLK_DOWN, false, false, 0);
    (void)tr(&s, &e, 3.5);
    CHECK(nk_ui_input_poll(&s.input, 9.0, g_out, NK_UI_INPUT_MAX_EVENTS) == 0u, "key release stops it");
    e = key_event(SDLK_Z, true, false, 0);
    CHECK(tr(&s, &e, 4.0) == 0u, "an unmapped key is ignored");

    /* Gamepad: with no open pad the south label is unknown, so the family is neutral. */
    e = pad_button(SDL_GAMEPAD_BUTTON_SOUTH, true);
    n = tr(&s, &e, 5.0);
    CHECK(n == 2 && g_out[0].type == NK_UI_EV_DEVICE && g_out[0].device == 41 &&
          g_out[0].pad_family == NK_UI_PAD_NEUTRAL && g_out[1].type == NK_UI_EV_ACTIVATE,
          "south confirms, neutral family for an unknown label");
    e = pad_button(SDL_GAMEPAD_BUTTON_SOUTH, false);
    (void)tr(&s, &e, 5.1);
    e = pad_button(SDL_GAMEPAD_BUTTON_EAST, true);
    n = tr(&s, &e, 5.2);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_BACK, "east goes back");
    e = pad_button(SDL_GAMEPAD_BUTTON_WEST, true);
    n = tr(&s, &e, 5.3);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_OPTIONS, "west is options");
    e = pad_button(SDL_GAMEPAD_BUTTON_NORTH, true);
    n = tr(&s, &e, 5.4);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_DETAILS, "north is details");
    e = pad_button(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, true);
    n = tr(&s, &e, 5.5);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_TAB && g_out[0].dir == NK_UI_DIR_LEFT, "left shoulder");
    e = pad_button(SDL_GAMEPAD_BUTTON_START, true);
    n = tr(&s, &e, 5.6);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_MENU, "start is the menu");
    e = pad_button(SDL_GAMEPAD_BUTTON_GUIDE, true);
    CHECK(tr(&s, &e, 5.7) == 0u, "Guide is not a UI button");
    e = pad_button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, true);
    n = tr(&s, &e, 6.0);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_NAV && g_out[0].dir == NK_UI_DIR_RIGHT, "d-pad right");
    e = pad_button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT, false);
    (void)tr(&s, &e, 6.1);

    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_GAMEPAD_AXIS_MOTION;
    e.gaxis.which = 41;
    e.gaxis.axis = SDL_GAMEPAD_AXIS_LEFTX;
    e.gaxis.value = 20000;
    n = tr(&s, &e, 7.0);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_NAV && g_out[0].dir == NK_UI_DIR_RIGHT, "stick right past 0.5");
    e.gaxis.value = 5000;
    CHECK(tr(&s, &e, 7.1) == 0u, "stick back inside the dead zone releases");
    e.gaxis.axis = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
    e.gaxis.value = 30000;
    CHECK(tr(&s, &e, 7.2) == 0u, "triggers are ignored");

    /* Hot-plug through SDL's own events: a pad that connects switches the whole
       prompt bar to controller glyphs, and its removal switches back. */
    {
        NkUiSdlInput plug;
        nk_ui_sdl_input_init(&plug);
        CHECK(nk_ui_input_prompt_style(&plug.input) == NK_UI_PROMPT_KEYCAPS,
              "no pad at start: keyboard and mouse glyphs");
        memset(&e, 0, sizeof(e));
        e.type = SDL_EVENT_GAMEPAD_ADDED;
        e.gdevice.which = 31;
        (void)tr(&plug, &e, 7.5);
        CHECK(nk_ui_input_prompt_style(&plug.input) != NK_UI_PROMPT_KEYCAPS &&
              nk_ui_input_pad_connected(&plug.input),
              "SDL_EVENT_GAMEPAD_ADDED switches to controller glyphs");
        e.type = SDL_EVENT_GAMEPAD_REMOVED;
        (void)tr(&plug, &e, 7.6);
        CHECK(nk_ui_input_prompt_style(&plug.input) == NK_UI_PROMPT_KEYCAPS &&
              !nk_ui_input_pad_connected(&plug.input),
              "SDL_EVENT_GAMEPAD_REMOVED switches back to keyboard and mouse");
    }

    /* Mouse and wheel, scaled into logical pixels. */
    nk_ui_sdl_input_set_event_scale(&s, 0.5f);
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_MOUSE_MOTION;
    e.motion.x = 200.0f;
    e.motion.y = 100.0f;
    n = tr(&s, &e, 8.0);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_POINTER && g_out[0].x == 100.0f &&
          g_out[0].y == 50.0f && !g_out[0].down, "mouse motion in logical pixels");
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    e.button.button = SDL_BUTTON_LEFT;
    e.button.down = true;
    e.button.x = 200.0f;
    e.button.y = 100.0f;
    n = tr(&s, &e, 8.1);
    CHECK(n == 2 && g_out[0].type == NK_UI_EV_DEVICE && g_out[0].pad_family == NK_UI_PAD_KEYBOARD &&
          g_out[1].type == NK_UI_EV_POINTER && g_out[1].down,
          "a left click announces the keyboard and mouse, then presses");
    e.button.button = SDL_BUTTON_RIGHT;
    CHECK(tr(&s, &e, 8.2) == 0u, "other buttons are ignored");
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.y = 0.5f;
    e.wheel.mouse_x = 200.0f;
    e.wheel.mouse_y = 100.0f;
    CHECK(tr(&s, &e, 8.3) == 0u, "half a notch waits");
    n = tr(&s, &e, 8.4);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_WHEEL && g_out[0].dir == NK_UI_DIR_UP &&
          g_out[0].x == 100.0f && g_out[0].y == 50.0f, "the second half completes a notch");
    e.wheel.y = -2.0f;
    n = tr(&s, &e, 8.5);
    CHECK(n == 2 && g_out[0].dir == NK_UI_DIR_DOWN && g_out[1].dir == NK_UI_DIR_DOWN,
          "two notches down");
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_MOUSE_WHEEL;
    e.wheel.x = 1.0f;
    e.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    n = tr(&s, &e, 8.6);
    CHECK(n == 1 && g_out[0].dir == NK_UI_DIR_LEFT, "flipped wheels reverse");

    /* Text. */
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_TEXT_INPUT;
    e.text.text = "\xC3\xA9!";
    n = tr(&s, &e, 9.0);
    CHECK(n == 2 && g_out[0].type == NK_UI_EV_TEXT && g_out[0].codepoint == 0xE9u &&
          g_out[1].codepoint == '!', "UTF-8 text becomes code points");

    /* Pad removal. */
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_GAMEPAD_REMOVED;
    e.gdevice.which = 41;
    n = tr(&s, &e, 10.0);
    CHECK(n == 1 && g_out[0].type == NK_UI_EV_DEVICE && !g_out[0].down && g_out[0].device == 41,
          "pad removal is reported");
    CHECK(tr(&s, NULL, 10.0) == 0u, "NULL event");
    memset(&e, 0, sizeof(e));
    e.type = SDL_EVENT_QUIT;
    CHECK(tr(&s, &e, 10.1) == 0u, "unrelated events are ignored");
    return 1;
}

#endif /* NK_UI_WITH_SDL3 */

static int run_sdl_leg(void) {
#ifdef NK_UI_WITH_SDL3
    if (!test_sdl_translation()) {
        return 1;
    }
    puts("nk_ui SDL input translation passed");
    return 0;
#else
    fprintf(stderr, "SKIP: SDL3 is not available to this build (compiled without "
            "-DNK_UI_WITH_SDL3), so the SDL_Event translation in nk_ui_sdl_input.c "
            "was not exercised\n");
    return 77;
#endif
}

int main(int argc, char **argv) {
    int i;
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sdl-input") == 0) {
            return run_sdl_leg();
        }
        if (strcmp(argv[i], "--print") == 0) {
            static char actual[65536];
            if (!build_glyph_goldens(actual, sizeof(actual))) {
                return 1;
            }
            fputs(actual, stdout);
            return 0;
        }
    }
    if (!test_grid() || !test_ragged_row() || !test_scroll_into_view() ||
        !test_dialog_trap_and_scope_memory() || !test_tab_cycle_and_repair() ||
        !test_repeat_timing() || !test_stick_hysteresis() || !test_event_construction() ||
        !test_prompt_style_follows_device() ||
        !test_utf8() || !test_label_table() || !test_prompt_bounds() ||
        !test_prompt_images() || !test_glyph_goldens()) {
        return 1;
    }
    puts("nk_ui focus graph, input model, label table, prompt glyph goldens and images passed");
    return 0;
}
