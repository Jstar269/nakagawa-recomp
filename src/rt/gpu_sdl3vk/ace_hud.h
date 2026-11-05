/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * ace_hud.h - the HUD mask and depth that Ace Mode's smooth motion reads besides a presented
 * frame's pixels (ace_smooth.h, AceSmoothAux; docs/ENHANCEMENT_CONTRACT.md section 10.6).
 *
 * The GPU GE (ge_gpu.c) notes the box of each through-mode (2D, pre-transformed) draw on the
 * target it draws into, and at a present publishes that target's boxes and its depth image. The
 * boxes become one bit per guest pixel of the 480x272 display. A box that covers a quarter of
 * the screen or more, or that samples a render target, is a backdrop or a post effect, not HUD,
 * and is not kept. A frame with more boxes than the list holds has no mask (today's behaviour).
 * Every box that is not kept, every frame without a mask or depth, and every pair that ran
 * without them is counted (AceSmoothAuxStats) for the Ace report and its exit line.
 *
 * Everything here is host-only bookkeeping: it reads what the GE already computed and changes
 * nothing the guest, the authentic renderer or a capture sees. The list code is plain C and
 * calls nothing of the runtime, so ace-smooth-selftest links it with the C library alone.
 */
#ifndef SR_ACE_HUD_H
#define SR_ACE_HUD_H

#include <stdint.h>

#include "ace_smooth.h"
#include "../ace.h"         /* AceSmoothAuxStats */

#define ACE_HUD_MAX_BOXES 1024u
/* A box this large or larger is not HUD: a quarter of the 480x272 screen. */
#define ACE_HUD_MAX_BOX_AREA ((float)(ACE_SMOOTH_HUD_W * ACE_SMOOTH_HUD_H) / 4.0f)
/* Boxes that together cover more than this many screens are not a HUD either. */
#define ACE_HUD_MAX_SCREENS 2.0f
/* How many render targets ace_hud keeps a list for (ge_gpu.c MAX_TGT is smaller). */
#define ACE_HUD_SLOTS 16

typedef struct {
    float x0, y0, x1, y1;           /* guest pixels, x0 < x1, y0 < y1, inside the screen */
} AceHudBox;

typedef struct {
    uint32_t count;
    int overflow;                   /* 1: more boxes than fit, or too much area: no mask */
    float area;
    AceHudBox box[ACE_HUD_MAX_BOXES];
} AceHudList;

/* What became of a box. */
typedef enum {
    ACE_HUD_OUTSIDE,                /* nothing on screen: off the edge, empty, or not a number */
    ACE_HUD_KEPT,
    ACE_HUD_MERGED,                 /* the same box as the one before: a quad's second triangle */
    ACE_HUD_LARGE,                  /* a quarter of the screen or more */
    ACE_HUD_TARGET,                 /* the draw samples a render target */
    ACE_HUD_FULL                    /* past ACE_HUD_MAX_BOXES or ACE_HUD_MAX_SCREENS: no mask */
} AceHudWhy;

void ace_hud_list_reset(AceHudList *l);
/* Adds one draw's box (any corner order, guest pixels). Returns 1 if it is kept as HUD (KEPT or
 * MERGED), and says why in `why` when that is not NULL. */
int ace_hud_list_add_why(AceHudList *l, float xa, float ya, float xb, float yb, int samples_target,
                         AceHudWhy *why);
int ace_hud_list_add(AceHudList *l, float xa, float ya, float xb, float yb, int samples_target);
/* Writes ACE_SMOOTH_HUD_WORDS words: a bit for every guest pixel a kept box touches. Returns 0,
 * with the words cleared, when the list overflowed: that frame has no mask. */
int ace_hud_list_bits(const AceHudList *l, uint32_t *bits);

/* ---- the runtime side (ace_hud.c; used by ge_gpu.c and gui.c) ---------------------------------- */

/* 1 when the HUD mask and depth are being collected: Ace smooth motion is on and neither
 * SR_ACE_SMOOTH_HUD=0 nor SR_ACE_SMOOTH_DEPTH=0 turned both off. Cheap: read on every 2D draw. */
extern int g_ace_hud_on;
/* Called by gui.c when Ace smooth motion starts or stops. */
void ace_hud_enable(int on);
/* ge_gpu.c: a through-mode draw's box on render-target slot `slot`. */
void ace_hud_note(int slot, float xa, float ya, float xb, float yb, int samples_target);
/* ge_gpu.c, just before target image `src` is presented: its boxes since its last present, and
 * its depth image. `depth_gen` counts the draws that wrote that depth image; depth is handed on
 * only if one did since that depth image was last handed on (whichever target it was presented
 * with), because otherwise it holds an older frame's depth, not this picture's. */
void ace_hud_present(int slot, VkImage src, VkImage depth, VkImageLayout depth_layout, int depth_w,
                     int depth_h, uint32_t depth_gen);
/* gui.c's show pass, for the image `src` it is showing: what was published for that image, or
 * NULL (a CPU frame, the feature off, or nothing published for it). It always clears what was
 * published, so an aux never reaches another image. Valid until the next ace_hud_present. */
const AceSmoothAux *ace_hud_take(VkImage src);
/* The box and frame counts since ace_hud_enable (the pair counts are ace_smooth.c's). */
void ace_hud_stats(AceSmoothAuxStats *out);

#endif /* SR_ACE_HUD_H */
