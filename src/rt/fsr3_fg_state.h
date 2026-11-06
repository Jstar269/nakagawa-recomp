/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * fsr3_fg_state.h - when FSR 3 frame generation runs, with which frame ID and reset, and which image
 * each flip and VBLANK presents (docs: the FSR 3 design, section 10.2).
 *
 * Pure C, no GPU. The pacing is Smooth Motion's (ENHANCEMENT_CONTRACT.md section 10.6): the flip of
 * real frame B shows the midpoint of A and B, the next VBLANK shows B, and a flip that arrives first
 * replaces the held frame. Frame generation adds:
 *
 * - It runs only while the 30 Hz detector holds the title at 30. A title whose flips arrive every
 *   VBLANK (60 Hz) is refused with one line, FSR3FG_REFUSED_60_LINE, and shows its frames as they
 *   come; so does every other class. Nothing runs that could drop or double an image.
 * - It runs on every new real frame at 30 Hz that went through FSR 3, so the next frame always has a
 *   predecessor. Frame IDs step by exactly one per new real frame.
 * - A run resets when frame generation did not run on the previous real frame (the first frame, a
 *   frame that fell back to FSR 1, a frame at another rate), at a cut, and when the frame's camera is
 *   not agreed. A reset's output is never shown.
 * - A 30 Hz pair with no generated image keeps the cadence: its flip shows the previous real frame
 *   again and the next VBLANK shows the new one, as Smooth Motion does at a cut. Every such pair is
 *   counted by reason.
 */
#ifndef SR_FSR3_FG_STATE_H
#define SR_FSR3_FG_STATE_H

#include <stdint.h>

#include "ace.h"

#define FSR3FG_REFUSED_60_LINE "fsr3fg needs a 30 Hz title until PR-F"

/* One flip, as the presenter sees it. */
typedef struct {
    int new_frame; /* the flip carries a new real frame (ace_flip's verdict) */
    AceRate rate;  /* the 30 Hz detector's stable class at this flip */
    int fsr3;      /* the frame went through FSR 3's upscaler and has its shared resources (0: FSR 1) */
    int cut;       /* the frame is a different picture from the previous real frame */
    int camera;    /* the frame's camera is agreed (MotionFrame or the title hint) */
} Fsr3FgFlip;

typedef enum {
    FSR3FG_PRESENT_REAL,     /* the flip's own frame, now */
    FSR3FG_PRESENT_MIDPOINT, /* the generated midpoint now; the flip's frame waits for the next VBLANK */
    FSR3FG_PRESENT_PREVIOUS, /* the previous real frame again now; the flip's frame waits for the next VBLANK */
} Fsr3FgPresent;

/* Why a flip shows no generated image. */
typedef enum {
    FSR3FG_WHY_MIDPOINT = 0, /* it does show one */
    FSR3FG_WHY_REPEAT,       /* no new frame */
    FSR3FG_WHY_REFUSED_60,   /* the title runs at 60 */
    FSR3FG_WHY_NOT_30,       /* another class: 20 or 15 Hz, variable, static, or not yet settled */
    FSR3FG_WHY_FSR1,         /* the frame fell back to FSR 1: nothing to generate from */
    FSR3FG_WHY_RESET,        /* frame generation did not run on the previous real frame */
    FSR3FG_WHY_CUT,          /* a cut between the two frames */
    FSR3FG_WHY_CAMERA,       /* the frame's camera is not agreed (camera_ambiguous) */
    FSR3FG_WHY_COUNT
} Fsr3FgWhy;

typedef struct {
    Fsr3FgPresent present;
    Fsr3FgWhy why;
    int run;           /* prepare and dispatch frame generation on this frame */
    uint64_t frame_id; /* the frame's ID, when run */
    int reset;         /* the dispatch resets, when run */
    int superseded;    /* this flip replaced a frame still held */
    const char *line;  /* a line to print now, or NULL: the 60 Hz refusal, once a run */
} Fsr3FgDecision;

typedef struct {
    uint64_t next_frame_id;  /* the ID the next new real frame gets */
    int have_previous;       /* a real frame has been shown */
    int ran_previous;        /* frame generation ran on the previous new real frame */
    int held;                /* a real frame waits for its VBLANK */
    uint32_t held_after;     /* the VCOUNT of the flip that holds it */
    int refused_line_said;
    /* The run's counters. */
    uint64_t why[FSR3FG_WHY_COUNT]; /* per flip; why[FSR3FG_WHY_MIDPOINT] counts the midpoints shown */
    uint64_t runs, resets, held_shown, superseded;
} Fsr3FgState;

void fsr3_fg_state_reset(Fsr3FgState *s);

/* A flip at VBLANK count `vcount`. */
Fsr3FgDecision fsr3_fg_state_flip(Fsr3FgState *s, const Fsr3FgFlip *f, uint32_t vcount);

/* A VBLANK. 1 when the held frame is due now; it is then no longer held. */
int fsr3_fg_state_vblank(Fsr3FgState *s, uint32_t vcount);

const char *fsr3_fg_why_name(Fsr3FgWhy why);

#endif /* SR_FSR3_FG_STATE_H */
