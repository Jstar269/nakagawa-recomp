// SPDX-License-Identifier: GPL-3.0-only
// ****************************************************************************
// * This file is part of the xBRZ project. It is distributed under           *
// * GNU General Public License: https://www.gnu.org/licenses/gpl-3.0         *
// * Copyright (C) Zenju (zenju AT gmx DOT de) - All Rights Reserved          *
// *                                                                          *
// * Additionally and as a special exception, the author gives permission     *
// * to link the code of this program with the following libraries            *
// * (or with modified versions that use the same licenses), and distribute   *
// * linked combinations including the two: MAME, FreeFileSync, Snes9x, ePSXe *
// *                                                                          *
// * You must obey the GNU General Public License in all respects for all of  *
// * the code used other than MAME, FreeFileSync, Snes9x, ePSXe.              *
// * If you modify this file, you may extend this exception to your version   *
// * of the file, but you are not obligated to do so. If you do not wish to   *
// * do so, delete this exception statement from your version.                *
// ****************************************************************************
//
// Modified by Nakagawa Recomp contributors, 2026-11-04: translated from C++ to C (xBRZ 1.9,
// xbrz.h, xbrz_config.h and the parts of xbrz_tools.h the scaler uses). Only the
// ColorFormat::argbUnbuffered scaler is kept; see PROVENANCE.md in this directory.

#ifndef NK_XBRZ_H
#define NK_XBRZ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*  -------------------------------------------------------------------------
    | xBRZ: "Scale by rules" - high quality image upscaling filter by Zenju |
    -------------------------------------------------------------------------
    using a modified approach of xBR: https://forums.libretro.com/t/xbr-algorithm-tutorial/123
    - new rule set preserving small image features
    - support alpha channel
    - support processing image slices
    - support scaling up to 6xBRZ                                             */

#define NK_XBRZ_SCALE_FACTOR_MAX 6

/* xbrz::ScalerCfg with its defaults (xbrz_config.h). */
typedef struct NkXbrzCfg {
    double equalColorTolerance;        /* 30 */
    double centerDirectionBias;        /* 4 */
    double dominantDirectionThreshold; /* 3.6 */
    double steepDirectionThreshold;    /* 2.4 */
    double testAttribute;              /* 0: unused */
} NkXbrzCfg;

void nk_xbrz_default_cfg(NkXbrzCfg *cfg);

/*
-> map source (srcWidth * srcHeight) to target (scale * width x scale * height) image, optionally processing a
   half-open slice of rows [yFirst, yLast) only; yLast = INT32_MAX (or any value >= srcHeight) for all rows
-> pixels are ARGB (alpha in the high byte, BGRA byte order on little-endian machines), as
   xbrz::ColorFormat::argbUnbuffered: the colour distance is computed per pair, with no table
-> factor 1 copies the rows; factor 2 - NK_XBRZ_SCALE_FACTOR_MAX scales. Returns 0 for any other factor.

THREAD-SAFETY: - parts of the same image may be scaled by multiple threads as long as the [yFirst, yLast)
                 ranges do not overlap!
*/
int nk_xbrz_scale(int factor, const uint32_t *src, uint32_t *trg, int srcWidth, int srcHeight,
                  const NkXbrzCfg *cfg, int yFirst, int yLast);

#ifdef __cplusplus
}
#endif

#endif
