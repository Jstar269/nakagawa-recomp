// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Nakagawa-authored replacement for the configure-generated libavutil/avconfig.h of FFmpeg n4.4.
 * See src/rt/h264_ffmpeg_sub/PROVENANCE.md. */
#ifndef AVUTIL_AVCONFIG_H
#define AVUTIL_AVCONFIG_H
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define AV_HAVE_BIGENDIAN 1
#else
#define AV_HAVE_BIGENDIAN 0
#endif
#define AV_HAVE_FAST_UNALIGNED 0
#endif /* AVUTIL_AVCONFIG_H */
