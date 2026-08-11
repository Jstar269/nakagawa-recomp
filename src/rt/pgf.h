// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
//
/*
 * A C port of PPSSPP's Core/Font/PGF.cpp, enough to serve sceLibFont for ACX: parse a PSP .pgf
 * firmware font, report per-character metrics, and rasterise real glyph bitmaps into the guest
 * buffer the game uploads as a GE texture. Replaces the earlier synthetic 5x7 fallback so the
 * full character set (Latin punctuation, symbols, CJK via jpn0.pgf) renders correctly.
 */
#ifndef SR_PGF_H
#define SR_PGF_H

#include "pgf_api.h"

/* Load and parse a .pgf file from the host filesystem. Returns NULL on failure. */

/* Parse a PGF from an in-memory buffer.  The returned object owns a private copy, so callers
 * may release or overwrite the source after this returns. */

/* Release a PGF returned by pgf_open() or pgf_open_memory(). */

/* True if the font has a real glyph for this Unicode code point. */

/* Write a PGFFontInfo (PPSSPP layout) to guest address `gfi`. */

/* Fill a PGFCharInfo (0x3c bytes, PPSSPP layout) at guest address `gci` for `charCode`, falling
 * back to `altCharCode`. Returns 1 if a glyph was found, 0 otherwise (charInfo zeroed). */

/* Read a GlyphImage (PPSSPP layout) from guest address `ggi` and rasterise the glyph for
 * `charCode` (or `altCharCode`) into its buffer. Returns 1 if drawn. */

/* Rasterise by the PGF's native glyph-table id.  This is the backend used by firmware paths
 * that already resolved a character through the cmap. */
#endif
