/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2026 the Nakagawa Recomp authors */
/*
 * osk_overlay_paint.h - draws the in-window keyboard (osk_overlay.h) over a guest frame.
 *
 * The presenter (gui.c) calls osk_overlay_paint on the host copy of the frame it is about to
 * present, so the keyboard reaches the window and the frame capture alike, and guest VRAM is
 * never written. The drawing uses SDL's software renderer on that frame, the same way the
 * presenter's performance HUD does.
 */
#ifndef SR_OSK_OVERLAY_PAINT_H
#define SR_OSK_OVERLAY_PAINT_H

#include <stdint.h>

#include "osk_overlay.h"

/* 1 when SDL can draw the keyboard on this host (probed once). The presenter takes the
 * keyboard only when this holds, so the native box stays the fallback where it cannot. */
int osk_overlay_paint_available(void);

/* Draw the keyboard over a w x h frame of little-endian XRGB words ((r << 16) | (g << 8) | b),
 * in place. Draws nothing when SDL cannot draw. */
void osk_overlay_paint(const OskOverlay *o, uint32_t *px, int w, int h);

#endif /* SR_OSK_OVERLAY_PAINT_H */
