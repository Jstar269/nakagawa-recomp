/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr3_fg_state_selftest.c - G-S: frame generation's state machine (src/rt/fsr3_fg_state.c) against a
 * table written from the FSR 3 design, section 10.2, and Smooth Motion's pacing
 * (ENHANCEMENT_CONTRACT.md section 10.6). Pure C, no GPU and no game data.
 *
 * Each scenario is a sequence of flips and VBLANKs from a reset state. Each flip row gives the
 * presenter's inputs and the decision the design asks for: which image the flip presents, why there
 * is no generated image, whether frame generation runs and with which frame ID and reset, whether the
 * flip superseded a held frame and whether it prints the 60 Hz refusal. Each VBLANK row gives whether
 * the held frame is due. Every row must match, and two invariants hold on every flip: a midpoint is
 * never presented from a reset, and a run that does not reset has the ID one past the previous new
 * frame's, which frame generation also ran on.
 */
#include "fsr3_fg_state.h"

#include <stdio.h>
#include <string.h>

static int s_fail;

typedef struct {
    char kind;     /* 'F' a flip with a new frame, 'R' a repeat flip, 'V' a VBLANK */
    uint32_t vcount;
    AceRate rate;
    int fsr3, cut, camera;
    /* expected */
    Fsr3FgPresent present;
    Fsr3FgWhy why;
    int run;
    uint64_t frame_id;
    int reset, superseded, line;
    int due; /* V: the held frame is due */
} Row;

#define K1 ACE_RATE_K1
#define K2 ACE_RATE_K2
#define K3 ACE_RATE_K3
#define REAL FSR3FG_PRESENT_REAL
#define MID FSR3FG_PRESENT_MIDPOINT
#define PREV FSR3FG_PRESENT_PREVIOUS
/* A new frame at 30 Hz through FSR 3 with an agreed camera and no cut. */
#define F30(v, present, why, id, reset) { 'F', v, K2, 1, 0, 1, present, FSR3FG_WHY_##why, 1, id, reset, 0, 0, 0 }
/* A new frame with every input given. */
#define F(v, rate, fsr3, cut, cam, present, why, run, id, reset, sup, line) \
    { 'F', v, rate, fsr3, cut, cam, present, FSR3FG_WHY_##why, run, id, reset, sup, line, 0 }
#define REPEAT(v) { 'R', v, K2, 1, 0, 1, REAL, FSR3FG_WHY_REPEAT, 0, 0, 0, 0, 0, 0 }
#define V(v, due) { 'V', v, K2, 0, 0, 0, REAL, FSR3FG_WHY_MIDPOINT, 0, 0, 0, 0, 0, due }

typedef struct {
    uint64_t midpoints, runs, resets, held_shown, superseded;
} Totals;

typedef struct {
    const char *name;
    const Row *rows;
    size_t count;
    Totals totals;
} Scenario;

/* A 30 Hz title: flips on even VBLANKs. The first frame has no predecessor and is shown as it comes;
 * then each flip shows the midpoint and the next VBLANK the frame. A VBLANK at the flip's own count
 * does not present the held frame. */
static const Row steady_30[] = {
    F30(0, REAL, RESET, 1, 1), V(0, 0), V(1, 0),
    F30(2, MID, MIDPOINT, 2, 0), V(2, 0), V(3, 1),
    F30(4, MID, MIDPOINT, 3, 0), V(5, 1),
    F30(6, MID, MIDPOINT, 4, 0), V(7, 1), V(8, 0),
};

/* The detector settles on 30 after frames of no class: those show as they come and do not run
 * frame generation, but take frame IDs. The first 30 Hz frame resets and keeps the cadence. */
static const Row enter_30[] = {
    F(0, ACE_RATE_NONE, 1, 0, 1, REAL, NOT_30, 0, 0, 0, 0, 0), V(1, 0),
    F(2, ACE_RATE_NONE, 1, 0, 1, REAL, NOT_30, 0, 0, 0, 0, 0), V(3, 0),
    F30(4, PREV, RESET, 3, 1), V(5, 1),
    F30(6, MID, MIDPOINT, 4, 0), V(7, 1),
};

/* A cut: no midpoint across it. The cut frame runs with a reset, the previous frame is shown again
 * at its flip and the new one at the VBLANK, and the frame after it interpolates again. */
static const Row cut[] = {
    F30(0, REAL, RESET, 1, 1),
    F30(2, MID, MIDPOINT, 2, 0), V(3, 1),
    F(4, K2, 1, 1, 1, PREV, CUT, 1, 3, 1, 0, 0), V(5, 1),
    F30(6, MID, MIDPOINT, 4, 0), V(7, 1),
};

/* A frame whose camera is not agreed: no midpoint, a reset, counted. */
static const Row camera[] = {
    F30(0, REAL, RESET, 1, 1),
    F(2, K2, 1, 0, 0, PREV, CAMERA, 1, 2, 1, 0, 0), V(3, 1),
    F30(4, MID, MIDPOINT, 3, 0), V(5, 1),
};

/* A frame that fell back to FSR 1: frame generation does not run on it, but it takes its frame ID,
 * so the next frame's ID skips one and that frame resets. Neither pair shows a midpoint. */
static const Row fsr1_fallback[] = {
    F30(0, REAL, RESET, 1, 1),
    F30(2, MID, MIDPOINT, 2, 0), V(3, 1),
    F(4, K2, 0, 0, 1, PREV, FSR1, 0, 0, 0, 0, 0), V(5, 1),
    F30(6, PREV, RESET, 4, 1), V(7, 1),
    F30(8, MID, MIDPOINT, 5, 0), V(9, 1),
};

/* The first frame of the run fell back to FSR 1: nothing to show again, so it is shown as it comes. */
static const Row first_fsr1[] = {
    F(0, K2, 0, 0, 1, REAL, FSR1, 0, 0, 0, 0, 0), V(1, 0),
    F30(2, PREV, RESET, 2, 1), V(3, 1),
    F30(4, MID, MIDPOINT, 3, 0), V(5, 1),
};

/* A 60 Hz title: refused with one line, frames shown as they come, frame generation never runs. */
static const Row refused_60[] = {
    F(0, K1, 1, 0, 1, REAL, REFUSED_60, 0, 0, 0, 0, 1), V(0, 0),
    F(1, K1, 1, 0, 1, REAL, REFUSED_60, 0, 0, 0, 0, 0), V(1, 0),
    F(2, K1, 1, 0, 1, REAL, REFUSED_60, 0, 0, 0, 0, 0), V(2, 0),
};

/* 30, then 60, then 30. The first 60 Hz flip arrives while a frame is held: it supersedes it, is
 * refused from that flip and shown as it comes. Back at 30, frame generation resets. */
static const Row rate_change[] = {
    F30(0, REAL, RESET, 1, 1),
    F30(2, MID, MIDPOINT, 2, 0), V(3, 1),
    F30(4, MID, MIDPOINT, 3, 0),
    F(5, K1, 1, 0, 1, REAL, REFUSED_60, 0, 0, 0, 1, 1), V(5, 0),
    F(6, K1, 1, 0, 1, REAL, REFUSED_60, 0, 0, 0, 0, 0), V(6, 0),
    F30(7, PREV, RESET, 6, 1), V(8, 1),
    F30(9, MID, MIDPOINT, 7, 0), V(10, 1),
};

/* A new frame flips before the held frame's VBLANK: the held frame is never shown alone, and the
 * new flip interpolates from it, since frame generation ran on it. */
static const Row superseded[] = {
    F30(0, REAL, RESET, 1, 1),
    F30(2, MID, MIDPOINT, 2, 0),
    F(3, K2, 1, 0, 1, MID, MIDPOINT, 1, 3, 0, 1, 0), V(3, 0), V(4, 1),
};

/* A repeat flip while a frame is held shows that frame now, so its VBLANK has nothing to show; the
 * next new frame still interpolates. */
static const Row repeat[] = {
    F30(0, REAL, RESET, 1, 1),
    F30(2, MID, MIDPOINT, 2, 0), REPEAT(3), V(3, 0),
    F30(4, MID, MIDPOINT, 3, 0), V(5, 1),
    REPEAT(6), V(7, 0),
};

/* 20 Hz, variable and static titles show each frame as it comes, with no refusal line. */
static const Row other_rates[] = {
    F(0, K3, 1, 0, 1, REAL, NOT_30, 0, 0, 0, 0, 0), V(1, 0),
    F(3, K3, 1, 0, 1, REAL, NOT_30, 0, 0, 0, 0, 0), V(4, 0),
    F(6, ACE_RATE_VARIABLE, 1, 0, 1, REAL, NOT_30, 0, 0, 0, 0, 0),
    F(7, ACE_RATE_STATIC, 1, 0, 1, REAL, NOT_30, 0, 0, 0, 0, 0), V(8, 0),
};

#define SCENARIO(rows, mid, runs, resets, held, sup) \
    { #rows, rows, sizeof(rows) / sizeof(rows[0]), { mid, runs, resets, held, sup } }
static const Scenario k_scenarios[] = {
    SCENARIO(steady_30, 3, 4, 1, 3, 0),
    SCENARIO(enter_30, 1, 2, 1, 2, 0),
    SCENARIO(cut, 2, 4, 2, 3, 0),
    SCENARIO(camera, 1, 3, 2, 2, 0),
    SCENARIO(fsr1_fallback, 2, 4, 2, 4, 0),
    SCENARIO(first_fsr1, 1, 2, 1, 2, 0),
    SCENARIO(refused_60, 0, 0, 0, 0, 0),
    SCENARIO(rate_change, 3, 5, 2, 3, 1),
    SCENARIO(superseded, 2, 3, 1, 1, 1),
    SCENARIO(repeat, 2, 3, 1, 2, 0),
    SCENARIO(other_rates, 0, 0, 0, 0, 0),
};

#define EXPECT(scenario, row, field, got, want) do { \
        if ((uint64_t)(got) != (uint64_t)(want)) { \
            s_fail++; \
            fprintf(stderr, "FAIL %s row %u: %s %llu, want %llu\n", scenario, (unsigned)(row), field, \
                    (unsigned long long)(got), (unsigned long long)(want)); \
        } \
    } while (0)

static void run_scenario(const Scenario *sc) {
    Fsr3FgState s;
    fsr3_fg_state_reset(&s);
    uint64_t last_new_id = 0;
    int last_new_ran = 0;
    for (size_t i = 0; i < sc->count; i++) {
        const Row *r = &sc->rows[i];
        if (r->kind == 'V') {
            EXPECT(sc->name, i, "vblank due", fsr3_fg_state_vblank(&s, r->vcount), r->due);
            continue;
        }
        Fsr3FgFlip f = { r->kind == 'F', r->rate, r->fsr3, r->cut, r->camera };
        const Fsr3FgDecision d = fsr3_fg_state_flip(&s, &f, r->vcount);
        EXPECT(sc->name, i, "present", d.present, r->present);
        EXPECT(sc->name, i, "why", d.why, r->why);
        EXPECT(sc->name, i, "run", d.run, r->run);
        EXPECT(sc->name, i, "frame id", d.frame_id, r->frame_id);
        EXPECT(sc->name, i, "reset", d.reset, r->reset);
        EXPECT(sc->name, i, "superseded", d.superseded, r->superseded);
        EXPECT(sc->name, i, "line", d.line != NULL, r->line);
        if (d.line) EXPECT(sc->name, i, "line text", strcmp(d.line, "fsr3fg needs a 30 Hz title until PR-F"), 0);
        /* The invariants, whatever the table says. */
        if (d.present == FSR3FG_PRESENT_MIDPOINT)
            EXPECT(sc->name, i, "a midpoint from a run that did not reset", d.run && !d.reset, 1);
        if (d.present != FSR3FG_PRESENT_REAL) EXPECT(sc->name, i, "a held frame at 30 Hz", r->rate, ACE_RATE_K2);
        if (r->kind == 'F') {
            if (d.run && !d.reset) {
                EXPECT(sc->name, i, "a run without a reset follows a run", last_new_ran, 1);
                EXPECT(sc->name, i, "a run without a reset steps the frame ID by one", d.frame_id, last_new_id + 1);
            }
            last_new_ran = d.run;
            last_new_id = d.run ? d.frame_id : last_new_id + 1;
        }
    }
    EXPECT(sc->name, sc->count, "midpoints", s.why[FSR3FG_WHY_MIDPOINT], sc->totals.midpoints);
    EXPECT(sc->name, sc->count, "runs", s.runs, sc->totals.runs);
    EXPECT(sc->name, sc->count, "resets", s.resets, sc->totals.resets);
    EXPECT(sc->name, sc->count, "held frames shown", s.held_shown, sc->totals.held_shown);
    EXPECT(sc->name, sc->count, "superseded", s.superseded, sc->totals.superseded);
    uint64_t flips = 0, counted = 0;
    for (size_t i = 0; i < sc->count; i++) flips += sc->rows[i].kind != 'V';
    for (int w = 0; w < FSR3FG_WHY_COUNT; w++) counted += s.why[w];
    EXPECT(sc->name, sc->count, "every flip counted once", counted, flips);
}

int main(void) {
    size_t rows = 0;
    for (size_t i = 0; i < sizeof(k_scenarios) / sizeof(k_scenarios[0]); i++) {
        run_scenario(&k_scenarios[i]);
        rows += k_scenarios[i].count;
    }
    for (int w = 0; w < FSR3FG_WHY_COUNT; w++)
        if (!fsr3_fg_why_name((Fsr3FgWhy)w)[0] || fsr3_fg_why_name((Fsr3FgWhy)w)[0] == '?') {
            s_fail++;
            fprintf(stderr, "FAIL why %d has no name\n", w);
        }
    printf("fsr3_fg_state_selftest: G-S, %u scenarios, %u rows: %s\n",
           (unsigned)(sizeof(k_scenarios) / sizeof(k_scenarios[0])), (unsigned)rows, s_fail ? "FAIL" : "ok");
    return s_fail ? 1 : 0;
}
