/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_hud.c - see ace_hud.h.
 */
#include "ace_hud.h"

#include <math.h>
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

int ace_hud_list_add(AceHudList *l, float xa, float ya, float xb, float yb, int samples_target) {
    if (samples_target || l->overflow) return 0;
    if (!(xa == xa) || !(ya == ya) || !(xb == xb) || !(yb == yb)) return 0;   /* NaN */
    float x0 = clampf(fminf(xa, xb), 0.0f, (float)ACE_SMOOTH_HUD_W);
    float x1 = clampf(fmaxf(xa, xb), 0.0f, (float)ACE_SMOOTH_HUD_W);
    float y0 = clampf(fminf(ya, yb), 0.0f, (float)ACE_SMOOTH_HUD_H);
    float y1 = clampf(fmaxf(ya, yb), 0.0f, (float)ACE_SMOOTH_HUD_H);
    if (!(x1 > x0) || !(y1 > y0)) return 0;
    float area = (x1 - x0) * (y1 - y0);
    if (area >= ACE_HUD_MAX_BOX_AREA) return 0;
    /* A quad drawn as two triangles gives the same box twice in a row. */
    if (l->count) {
        const AceHudBox *last = &l->box[l->count - 1];
        if (last->x0 == x0 && last->y0 == y0 && last->x1 == x1 && last->y1 == y1) return 1;
    }
    if (l->count >= ACE_HUD_MAX_BOXES ||
        l->area + area > ACE_HUD_MAX_SCREENS * (float)(ACE_SMOOTH_HUD_W * ACE_SMOOTH_HUD_H)) {
        l->overflow = 1;
        return 0;
    }
    AceHudBox *b = &l->box[l->count++];
    b->x0 = x0; b->y0 = y0; b->x1 = x1; b->y1 = y1;
    l->area += area;
    return 1;
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
static uint32_t s_depth_gen[ACE_HUD_SLOTS];
static int s_depth_seen[ACE_HUD_SLOTS];
static uint32_t s_bits[ACE_SMOOTH_HUD_WORDS];
static AceSmoothAux s_aux;
static int s_aux_ready;

static int env_off(const char *name) {
    const char *v = getenv(name);
    return v && v[0] == '0' && v[1] == '\0';
}

void ace_hud_enable(int on) {
    s_aux_ready = 0;
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
    g_ace_hud_on = s_lists != NULL;
}

void ace_hud_note(int slot, float xa, float ya, float xb, float yb, int samples_target) {
    if (!g_ace_hud_on || !s_use_hud || slot < 0 || slot >= ACE_HUD_SLOTS) return;
    (void)ace_hud_list_add(&s_lists[slot], xa, ya, xb, yb, samples_target);
}

void ace_hud_present(int slot, VkImage depth, VkImageLayout depth_layout, int depth_w, int depth_h,
                     uint32_t depth_gen) {
    s_aux_ready = 0;
    if (!g_ace_hud_on || slot < 0 || slot >= ACE_HUD_SLOTS) return;
    memset(&s_aux, 0, sizeof(s_aux));
    if (s_use_hud) {
        if (ace_hud_list_bits(&s_lists[slot], s_bits)) s_aux.hud_bits = s_bits;
        ace_hud_list_reset(&s_lists[slot]);
    }
    int moved = !s_depth_seen[slot] || s_depth_gen[slot] != depth_gen;
    s_depth_seen[slot] = 1;
    s_depth_gen[slot] = depth_gen;
    if (s_use_depth && depth && moved) {
        s_aux.depth = depth;
        s_aux.depth_layout = depth_layout;
        s_aux.depth_w = depth_w;
        s_aux.depth_h = depth_h;
    }
    s_aux_ready = 1;
}

const AceSmoothAux *ace_hud_take(void) {
    if (!s_aux_ready) return NULL;
    s_aux_ready = 0;
    return &s_aux;
}
