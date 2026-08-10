/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored minimal stand-in for FFmpeg n4.4 libavutil/libm.h
 * (PR-A).
 *
 * Upstream libm.h provides architecture-dependent av_sin/av_cos/av_exp2
 * wrappers. The imported decoder subset only needs <math.h> and the M_PI
 * constants, so the header is reduced to those. See
 * src/rt/atrac3p/PROVENANCE.md.
 */

#ifndef AT3P_LIBAVUTIL_LIBM_H
#define AT3P_LIBAVUTIL_LIBM_H

#include <math.h>
#include "mathematics.h"

#endif /* AT3P_LIBAVUTIL_LIBM_H */
