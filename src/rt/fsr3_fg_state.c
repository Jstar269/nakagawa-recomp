/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr3_fg_state.c - frame generation's run, reset and pacing decisions (fsr3_fg_state.h).
 */
#include "fsr3_fg_state.h"

#include <string.h>

void fsr3_fg_state_reset(Fsr3FgState *s) {
    memset(s, 0, sizeof(*s));
    s->next_frame_id = 1;
}

Fsr3FgDecision fsr3_fg_state_flip(Fsr3FgState *s, const Fsr3FgFlip *f, uint32_t vcount) {
    Fsr3FgDecision d;
    memset(&d, 0, sizeof(d));
    d.present = FSR3FG_PRESENT_REAL;

    if (!f->new_frame) {
        /* A repeat flip shows its own frame, which is the held one if a frame is held: it is shown
         * now instead of at the VBLANK. Frame generation does not run and the ID does not step. */
        if (s->held) s->held_shown++;
        s->held = 0;
        d.why = FSR3FG_WHY_REPEAT;
        s->why[d.why]++;
        return d;
    }

    /* A new frame supersedes a frame still held: that frame is never shown alone. */
    if (s->held) {
        d.superseded = 1;
        s->superseded++;
    }
    s->held = 0;

    const uint64_t id = s->next_frame_id++;
    const int at_30 = f->rate == ACE_RATE_K2;
    d.run = at_30 && f->fsr3;
    if (d.run) {
        d.frame_id = id;
        d.reset = !s->ran_previous || f->cut || !f->camera;
        s->runs++;
        if (d.reset) s->resets++;
    }

    if (!at_30) {
        d.why = f->rate == ACE_RATE_K1 ? FSR3FG_WHY_REFUSED_60 : FSR3FG_WHY_NOT_30;
        if (d.why == FSR3FG_WHY_REFUSED_60 && !s->refused_line_said) {
            d.line = FSR3FG_REFUSED_60_LINE;
            s->refused_line_said = 1;
        }
    } else if (!f->fsr3) {
        d.why = FSR3FG_WHY_FSR1;
    } else if (!s->ran_previous) {
        d.why = FSR3FG_WHY_RESET;
    } else if (f->cut) {
        d.why = FSR3FG_WHY_CUT;
    } else if (!f->camera) {
        d.why = FSR3FG_WHY_CAMERA;
    } else {
        d.why = FSR3FG_WHY_MIDPOINT;
    }

    if (at_30 && d.why == FSR3FG_WHY_MIDPOINT) {
        d.present = FSR3FG_PRESENT_MIDPOINT;
    } else if (at_30 && s->have_previous) {
        d.present = FSR3FG_PRESENT_PREVIOUS;
    }
    if (d.present != FSR3FG_PRESENT_REAL) {
        s->held = 1;
        s->held_after = vcount;
    }
    s->have_previous = 1;
    s->ran_previous = d.run;
    s->why[d.why]++;
    return d;
}

int fsr3_fg_state_vblank(Fsr3FgState *s, uint32_t vcount) {
    if (!s->held || vcount == s->held_after) return 0;
    s->held = 0;
    s->held_shown++;
    return 1;
}

const char *fsr3_fg_why_name(Fsr3FgWhy why) {
    static const char *const names[FSR3FG_WHY_COUNT] = {
        "midpoint", "repeat", "refused_60", "not_30", "fsr1_fallback", "reset", "cut", "camera_ambiguous",
    };
    return (unsigned)why < FSR3FG_WHY_COUNT ? names[why] : "?";
}
