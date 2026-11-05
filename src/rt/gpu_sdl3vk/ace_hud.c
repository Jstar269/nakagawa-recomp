/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_hud.c - see ace_hud.h.
 */
#include "ace_hud.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ace_hud_list_reset(AceHudList *l) {
    l->count = 0;
    l->overflow = 0;
    l->area = 0.0f;
}

static float clampf(float v, float lo, float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

int ace_hud_list_add_why(AceHudList *l, float xa, float ya, float xb, float yb, int samples_target,
                         AceHudWhy *why) {
    AceHudWhy w;
    if (samples_target) w = ACE_HUD_TARGET;
    else if (l->overflow) w = ACE_HUD_FULL;
    else if (!(xa == xa) || !(ya == ya) || !(xb == xb) || !(yb == yb)) w = ACE_HUD_OUTSIDE;   /* NaN */
    else {
        float x0 = clampf(fminf(xa, xb), 0.0f, (float)ACE_SMOOTH_HUD_W);
        float x1 = clampf(fmaxf(xa, xb), 0.0f, (float)ACE_SMOOTH_HUD_W);
        float y0 = clampf(fminf(ya, yb), 0.0f, (float)ACE_SMOOTH_HUD_H);
        float y1 = clampf(fmaxf(ya, yb), 0.0f, (float)ACE_SMOOTH_HUD_H);
        float area = (x1 - x0) * (y1 - y0);
        const AceHudBox *last = l->count ? &l->box[l->count - 1] : NULL;
        if (!(x1 > x0) || !(y1 > y0)) w = ACE_HUD_OUTSIDE;
        else if (area >= ACE_HUD_MAX_BOX_AREA) w = ACE_HUD_LARGE;
        /* A quad drawn as two triangles gives the same box twice in a row. */
        else if (last && last->x0 == x0 && last->y0 == y0 && last->x1 == x1 && last->y1 == y1)
            w = ACE_HUD_MERGED;
        else if (l->count >= ACE_HUD_MAX_BOXES ||
                 l->area + area > ACE_HUD_MAX_SCREENS * (float)(ACE_SMOOTH_HUD_W * ACE_SMOOTH_HUD_H)) {
            l->overflow = 1;
            w = ACE_HUD_FULL;
        } else {
            AceHudBox *b = &l->box[l->count++];
            b->x0 = x0; b->y0 = y0; b->x1 = x1; b->y1 = y1;
            l->area += area;
            w = ACE_HUD_KEPT;
        }
    }
    if (why) *why = w;
    return w == ACE_HUD_KEPT || w == ACE_HUD_MERGED;
}

int ace_hud_list_add(AceHudList *l, float xa, float ya, float xb, float yb, int samples_target) {
    return ace_hud_list_add_why(l, xa, ya, xb, yb, samples_target, NULL);
}

int ace_hud_list_bits(const AceHudList *l, uint32_t *bits) {
    memset(bits, 0, ACE_SMOOTH_HUD_WORDS * sizeof(uint32_t));
    if (l->overflow) return 0;
    for (uint32_t i = 0; i < l->count; i++) {
        const AceHudBox *b = &l->box[i];
        /* Every pixel the box touches: a HUD's edges are often filtered or blended. */
        int x0 = (int)floorf(b->x0), x1 = (int)ceilf(b->x1);
        int y0 = (int)floorf(b->y0), y1 = (int)ceilf(b->y1);
        for (int y = y0; y < y1; y++) {
            uint32_t row = (uint32_t)y * ACE_SMOOTH_HUD_W;
            for (int x = x0; x < x1; x++) {
                uint32_t k = row + (uint32_t)x;
                bits[k >> 5] |= 1u << (k & 31u);
            }
        }
    }
    return 1;
}

/* ---- the runtime side ------------------------------------------------------------------------ */

int g_ace_hud_on;
static int s_use_hud, s_use_depth;
static AceHudList *s_lists;              /* ACE_HUD_SLOTS of them, made when enabled */
/* Each depth image's write count when it was last handed on. Keyed by the image, not by the
 * colour target: two display buffers often share one depth buffer, and the second's present
 * must not hand on depth the first frame wrote. */
static struct { VkImage image; uint32_t gen; } s_depth_seen[ACE_HUD_SLOTS];
static unsigned s_depth_next;
static uint32_t s_bits[ACE_SMOOTH_HUD_WORDS];
static AceSmoothAux s_aux;
static VkImage s_aux_src;                /* the image the published aux belongs to */
static int s_aux_ready;
static AceSmoothAuxStats s_stats;
static int s_warned_full;

static int env_off(const char *name) {
    const char *v = getenv(name);
    return v && v[0] == '0' && v[1] == '\0';
}

void ace_hud_enable(int on) {
    s_aux_ready = 0;
    s_aux_src = VK_NULL_HANDLE;
    memset(&s_stats, 0, sizeof(s_stats));
    s_warned_full = 0;
    if (!on) {
        g_ace_hud_on = 0;
        free(s_lists);
        s_lists = NULL;
        return;
    }
    s_use_hud = !env_off("SR_ACE_SMOOTH_HUD");
    s_use_depth = !env_off("SR_ACE_SMOOTH_DEPTH");
    if (!s_use_hud && !s_use_depth) { g_ace_hud_on = 0; return; }
    if (!s_lists) s_lists = (AceHudList *)calloc(ACE_HUD_SLOTS, sizeof(AceHudList));
    memset(s_depth_seen, 0, sizeof(s_depth_seen));
    s_depth_next = 0;
    g_ace_hud_on = s_lists != NULL;
}

void ace_hud_note(int slot, float xa, float ya, float xb, float yb, int samples_target) {
    if (!g_ace_hud_on || !s_use_hud || slot < 0 || slot >= ACE_HUD_SLOTS) return;
    AceHudWhy why;
    (void)ace_hud_list_add_why(&s_lists[slot], xa, ya, xb, yb, samples_target, &why);
    switch (why) {
    case ACE_HUD_KEPT: s_stats.boxes_kept++; break;
    case ACE_HUD_MERGED: s_stats.boxes_merged++; break;
    case ACE_HUD_LARGE: s_stats.boxes_large++; break;
    case ACE_HUD_TARGET: s_stats.boxes_target++; break;
    case ACE_HUD_OUTSIDE: s_stats.boxes_outside++; break;
    case ACE_HUD_FULL: s_stats.boxes_full++; break;
    }
}

/* 1 when a draw wrote `depth` since it was last handed on; records `gen` as handed on. */
static int depth_fresh(VkImage depth, uint32_t gen) {
    for (unsigned i = 0; i < ACE_HUD_SLOTS; i++) {
        if (s_depth_seen[i].image != depth) continue;
        int fresh = s_depth_seen[i].gen != gen;
        s_depth_seen[i].gen = gen;
        return fresh;
    }
    s_depth_seen[s_depth_next].image = depth;
    s_depth_seen[s_depth_next].gen = gen;
    s_depth_next = (s_depth_next + 1u) % ACE_HUD_SLOTS;
    return 1;
}

void ace_hud_present(int slot, VkImage src, VkImage depth, VkImageLayout depth_layout, int depth_w,
                     int depth_h, uint32_t depth_gen) {
    if (s_aux_ready) s_stats.published_unshown++;
    s_aux_ready = 0;
    s_aux_src = VK_NULL_HANDLE;
    if (!g_ace_hud_on || slot < 0 || slot >= ACE_HUD_SLOTS || !src) return;
    memset(&s_aux, 0, sizeof(s_aux));
    if (s_use_hud) {
        if (ace_hud_list_bits(&s_lists[slot], s_bits)) {
            s_aux.hud_bits = s_bits;
            s_stats.frames_mask++;
        } else {
            s_stats.frames_overflow++;
            if (!s_warned_full) {
                s_warned_full = 1;
                fprintf(stderr, "ACE_MODE: smooth motion: a frame drew more than %u HUD boxes, or more than "
                                "two screens of them, so frames like it get no HUD mask (counted in the Ace "
                                "report)\n", ACE_HUD_MAX_BOXES);
            }
        }
        ace_hud_list_reset(&s_lists[slot]);
    }
    if (s_use_depth) {
        if (!depth) {
            s_stats.frames_depth_none++;
        } else if (!depth_fresh(depth, depth_gen)) {
            s_stats.frames_depth_unwritten++;
        } else {
            s_aux.depth = depth;
            s_aux.depth_layout = depth_layout;
            s_aux.depth_w = depth_w;
            s_aux.depth_h = depth_h;
            s_stats.frames_depth++;
        }
    }
    s_aux_src = src;
    s_aux_ready = 1;
}

const AceSmoothAux *ace_hud_take(VkImage src) {
    if (!s_aux_ready) return NULL;
    s_aux_ready = 0;
    if (!src || src != s_aux_src) {
        s_stats.published_unshown++;
        return NULL;
    }
    return &s_aux;
}

void ace_hud_stats(AceSmoothAuxStats *out) {
    if (!out) return;
    out->boxes_kept = s_stats.boxes_kept;
    out->boxes_merged = s_stats.boxes_merged;
    out->boxes_large = s_stats.boxes_large;
    out->boxes_target = s_stats.boxes_target;
    out->boxes_outside = s_stats.boxes_outside;
    out->boxes_full = s_stats.boxes_full;
    out->frames_mask = s_stats.frames_mask;
    out->frames_overflow = s_stats.frames_overflow;
    out->frames_depth = s_stats.frames_depth;
    out->frames_depth_unwritten = s_stats.frames_depth_unwritten;
    out->frames_depth_none = s_stats.frames_depth_none;
    out->published_unshown = s_stats.published_unshown;
}
