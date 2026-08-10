/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored minimal stand-in for FFmpeg n4.4 libavutil/mathematics.h
 * (PR-A).
 *
 * The imported FFT/MDCT template code needs the M_PI family of constants.
 * Upstream mathematics.h provides them plus rational helpers that this
 * subset does not use. See src/rt/atrac3p/PROVENANCE.md.
 */

#ifndef AT3P_LIBAVUTIL_MATHEMATICS_H
#define AT3P_LIBAVUTIL_MATHEMATICS_H

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_PI_2
#define M_PI_2 1.57079632679489661923
#endif
#ifndef M_SQRT1_2
#define M_SQRT1_2 0.70710678118654752440
#endif

#endif /* AT3P_LIBAVUTIL_MATHEMATICS_H */
