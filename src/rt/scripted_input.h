/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */

/* Scripted controller input delivered in guest time.
 *
 * Every scripted input source in the runtime -- the SR_PADSCRIPT legacy table, the route
 * program's PRESS / DELAY / PRESS_UNTIL / PRESS_WHILE steps, and the headless auto-START
 * pulse -- used to decide the pad state from the VCOUNT seen at each serviced vblank. In
 * the default paced mode VCOUNT is host time, and a host that falls behind services
 * several elapsed periods at once at the batch's final VCOUNT. A four-vblank press could
 * then be latched on fewer samples or on none, and a guest that reads only the latest
 * sample, or has not started polling yet, never saw it. Whether a press arrived depended
 * on how fast the host ran.
 *
 * Here a script is a sequence of SEGMENTS: a button mask held for a nominal number of
 * controller samples (one sample is latched per serviced vblank). A segment ends only when
 *   - it has been latched for at least its nominal length, AND
 *   - the guest has read the controller and been handed a sample latched while it was
 *     current (sceCtrlReadBufferPositive / sceCtrlPeekBufferPositive).
 * Release gaps are segments too, so the guest also sees every release before the next
 * press, and a press is never merged with the next one. On a host that keeps up, the guest
 * reads during the nominal window and the timing is exactly the authored timing. On a
 * starved host the script stretches, never compresses, until the guest has seen each
 * segment. A segment may not begin before its nominal VCOUNT (`not_before`), so a script
 * never runs early.
 *
 * A pressed segment that the guest does not read within `budget` vblanks is OVERDUE: the
 * caller fails the run with a named reason. That is the bounded alternative to a press
 * silently falling on the floor. A gap segment waits without a budget: a guest that is
 * not polling cannot be pressed anyway, and nothing is lost while it waits.
 *
 * A segment's width is normally its nominal length in controller samples, which is vblank
 * time. A segment may instead state its width in GUEST READS (`reads` > 0): it is delivered
 * once that many guest reads have each been handed a sample latched while it was current.
 * A read counts once however many samples it returns, and reads that come before the
 * segment's first sample is latched do not count. On a starved host such a press lasts the
 * same number of guest reads however many vblanks the batch covered, which a vblank width
 * cannot promise. The budget of a pressed segment bounds the whole wait for its reads, so a
 * guest that reads once and then stops is an overdue press, not a hang.
 *
 * This header is pure logic with no runtime dependencies, so the exact state machine the
 * runtime uses (src/rt/hle.c) is also compiled by tools/test_scripted_input.py. It is
 * included by that one runtime translation unit; the functions are static for that
 * reason. Delivery ids wrap after 2^32 - 1 segments, which no run reaches. */
#ifndef SR_SCRIPTED_INPUT_H
#define SR_SCRIPTED_INPUT_H

#include <stdint.h>

/* Vblanks a pressed segment may wait for the guest to read it (SR_PADSCRIPT_READ_BUDGET
 * overrides it). Thirty seconds of display time: a guest that ignores the pad for longer
 * than that while a scripted press is held is not going to see it. */
#define SR_INPUT_DEFAULT_READ_BUDGET 1800u

typedef struct {
    uint32_t mask;        /* buttons held while this segment is current (0 = a release gap) */
    uint32_t samples;     /* nominal length in controller samples, >= 1 (vblank width) */
    uint32_t not_before;  /* earliest VCOUNT at which the segment may become current */
    uint32_t budget;      /* vblanks it may wait for a guest read; 0 = wait without limit */
    uint32_t reads;       /* guest-read width: reads that must observe it; 0 = vblank width */
} SrInputSegment;

typedef struct {
    SrInputSegment seg;
    int      current;     /* seg is being delivered */
    uint32_t id;          /* delivery id stamped into every sample latched while current */
    uint32_t latched;     /* samples latched while current */
    uint32_t since;       /* VCOUNT at which seg became current */
    uint32_t last_id;     /* last delivery id handed out */
    uint32_t read_id;     /* newest delivery id the guest has been handed by a read */
    uint32_t reads;       /* guest reads that observed the current segment */
} SrInputPlayer;

/* Stop delivering and forget the current segment. Delivery ids are never reused: samples
 * latched before a reset can still be sitting in the controller ring, and a stale sample
 * must never vouch for a segment started after it. */
static inline void sr_input_reset(SrInputPlayer *p) {
    uint32_t handed_out = p->last_id;
    SrInputPlayer zero = { { 0u, 0u, 0u, 0u, 0u }, 0, 0u, 0u, 0u, 0u, 0u, 0u };
    *p = zero;
    p->last_id = handed_out;
    p->read_id = handed_out;
}

/* Make `seg` current at VCOUNT `v`, abandoning whatever was current. */
static inline void sr_input_start(SrInputPlayer *p, const SrInputSegment *seg, uint32_t v) {
    p->seg = *seg;
    if (p->seg.samples == 0u) p->seg.samples = 1u;
    p->current = 1;
    p->last_id = p->last_id == UINT32_MAX ? 1u : p->last_id + 1u;
    p->id = p->last_id;
    p->latched = 0u;
    p->reads = 0u;
    p->since = v;
}

/* Stop delivering: the pad returns to released and nothing is waiting to be read. */
static inline void sr_input_idle(SrInputPlayer *p) { p->current = 0; }

static inline uint32_t sr_input_mask(const SrInputPlayer *p) {
    return p->current ? p->seg.mask : 0u;
}

static inline int sr_input_was_read(const SrInputPlayer *p) {
    return p->current && p->read_id >= p->id;
}

/* The current segment has been latched for its nominal length and the guest has read it; a
 * guest-read width is delivered once its reads have observed it. */
static inline int sr_input_delivered(const SrInputPlayer *p) {
    if (!p->current) return 0;
    if (p->seg.reads != 0u) return p->reads >= p->seg.reads;
    return p->latched >= p->seg.samples && p->read_id >= p->id;
}

/* A pressed segment that the guest has still not read after its budget. A guest-read width
 * is overdue while fewer reads than it asked for have observed it, however many did. */
static inline int sr_input_overdue(const SrInputPlayer *p, uint32_t v) {
    if (!p->current || p->seg.mask == 0u || p->seg.budget == 0u) return 0;
    if (p->seg.reads != 0u) return p->reads < p->seg.reads && v - p->since >= p->seg.budget;
    return p->read_id < p->id && v - p->since >= p->seg.budget;
}

/* Once per latched controller sample: count it against the current segment and return the
 * delivery id the sample carries (0 when nothing scripted is current). */
static inline uint32_t sr_input_latch(SrInputPlayer *p) {
    if (!p->current) return 0u;
    if (p->latched < UINT32_MAX) p->latched++;
    return p->id;
}

/* Once per guest controller read, with the newest delivery id among the samples the read
 * returned. Returns 1 when this read is the first to hand the guest the current segment.
 * A read observes the current segment when it hands over one of its samples, and then it
 * counts once toward a guest-read width. */
static inline int sr_input_read(SrInputPlayer *p, uint32_t newest_id) {
    int first = p->current && p->read_id < p->id && newest_id >= p->id;
    if (p->current && newest_id >= p->id && p->reads < UINT32_MAX) p->reads++;
    if (newest_id > p->read_id) p->read_id = newest_id;
    return first;
}

/* Play a fixed list of segments. Call once per serviced vblank, before that vblank's sample
 * is latched; *pos is the index of the next segment to start. A delivered segment hands
 * over to the next one in the same vblank once that one is due, and keeps its own mask
 * until then, so contiguous segments never gain a release between them. Returns -1 when
 * the current segment is overdue (the caller fails the run), 1 once the whole list has
 * been delivered, and 0 otherwise. */
static inline int sr_input_list_tick(SrInputPlayer *p, const SrInputSegment *list, int n,
                                     int *pos, uint32_t v) {
    if (p->current) {
        if (sr_input_overdue(p, v)) return -1;
        if (!sr_input_delivered(p)) return 0;
        if (*pos >= n) { sr_input_idle(p); return 1; }
    } else if (*pos >= n) {
        return 1;
    }
    if (v < list[*pos].not_before) return 0;
    sr_input_start(p, &list[*pos], v);
    (*pos)++;
    return 0;
}

/* Turn absolute "press `mask` at sample `f` for `w` samples" rows (the SR_PADSCRIPT table)
 * into the segment list that reproduces them: the pad state is cut at every row start and
 * end, overlapping rows are held together, release gaps become their own segments, and a
 * gap before the first row lets the guest see the pad released before it is pressed.
 * Pressed segments get `budget`; gaps wait without one. Rows need not be sorted. Returns
 * the number of segments written, or -1 when `out` (capacity `max`) is too small; 2n + 1
 * always suffices. */
static inline int sr_input_from_rows(const uint32_t *f, const uint32_t *mask, const uint32_t *w,
                                     int n, uint32_t budget, SrInputSegment *out, int max) {
    int count = 0;
    uint32_t at = 0u;
    for (;;) {
        /* The next cut after `at`: the earliest row start or end beyond it. */
        uint32_t next = UINT32_MAX;
        int any = 0;
        for (int i = 0; i < n; i++) {
            if (w[i] == 0u) continue;
            uint32_t end = f[i] + w[i] < f[i] ? UINT32_MAX : f[i] + w[i];
            if (f[i] > at && f[i] < next) { next = f[i]; any = 1; }
            if (end > at && end < next) { next = end; any = 1; }
        }
        if (!any) break;
        uint32_t held = 0u;
        for (int i = 0; i < n; i++) {
            if (w[i] == 0u) continue;
            uint32_t end = f[i] + w[i] < f[i] ? UINT32_MAX : f[i] + w[i];
            if (f[i] <= at && at < end) held |= mask[i];
        }
        if (count > 0 && out[count - 1].mask == held) {
            out[count - 1].samples += next - at;
        } else {
            if (count >= max) return -1;
            out[count].mask = held;
            /* The leading gap only has to show the guest a released pad; the first press
             * is placed by its own not_before, however late the script's first vblank. */
            out[count].samples = (count == 0 && held == 0u) ? 1u : next - at;
            out[count].not_before = at;
            out[count].budget = held ? budget : 0u;
            out[count].reads = 0u;          /* a table row is a vblank width */
            count++;
        }
        at = next;
    }
    return count;
}

#endif /* SR_SCRIPTED_INPUT_H */
