/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored standalone-build configuration (PR-A).
 *
 * This header is OUR replacement for the configure-generated
 * libavutil/avconfig.h of FFmpeg n4.4.
 * See src/rt/atrac3p/PROVENANCE.md.
 */

#ifndef AT3P_LIBAVUTIL_AVCONFIG_H
#define AT3P_LIBAVUTIL_AVCONFIG_H

#define AV_HAVE_BIGENDIAN 0
#define AV_HAVE_FAST_UNALIGNED 0

/* configure-generated libavutil/avconfig.h emits:
 *   #define av_restrict restrict
 * (configure line 7592 of n4.4). libavutil/float_dsp.h and
 * libavutil/fixed_dsp.h rely on it. */
#define av_restrict restrict

#endif /* AT3P_LIBAVUTIL_AVCONFIG_H */
