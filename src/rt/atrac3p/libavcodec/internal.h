/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored minimal stand-in for FFmpeg n4.4 libavcodec/internal.h
 * (PR-A).
 *
 * Upstream internal.h is a large private-header aggregation. The imported
 * decoder subset uses exactly one symbol from it: avpriv_report_missing_feature
 * (called on the CH_UNIT_EXTENSION path and the GHA amplitude-mode-0 path).
 * Upstream implements it in libavcodec/utils.c; this header re-implements the
 * same observable behavior (an error-level log line) as a static inline so the
 * standalone subset needs no utils.c. See src/rt/atrac3p/PROVENANCE.md.
 */

#ifndef AT3P_LIBAVCODEC_INTERNAL_H
#define AT3P_LIBAVCODEC_INTERNAL_H

#include <stdarg.h>
#include "libavutil/log.h"
#include "libavutil/attributes.h"

static inline void avpriv_report_missing_feature(void *avcl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    av_log(avcl, AV_LOG_ERROR, "Missing feature: ");
    av_vlog(avcl, AV_LOG_ERROR, fmt, ap);
    va_end(ap);
}

/* Upstream defines ff_dlog in libavutil/internal.h:196-200 (debug-gated
 * av_log). The imported bitstream.c legacy VLC-build section uses it;
 * AV_DEBUG/DEBUG are not defined in this subset, so the default no-op form
 * is reproduced exactly. */
#ifdef DEBUG
#define ff_dlog(ctx, ...) av_log(ctx, AV_LOG_DEBUG, __VA_ARGS__)
#else
#define ff_dlog(ctx, ...) do { if (0) av_log(ctx, AV_LOG_DEBUG, __VA_ARGS__); } while (0)
#endif

/* Upstream implements avpriv_request_sample() in libavutil/log.c:474 via
 * missing_feature_sample(1, ...). This authored inline provides the same
 * observable behavior (an AV_LOG_ERROR diagnostic containing the message).
 * The call site in bitstream.c:263 is inside the FF_API_AVPRIV_PUT_BITS
 * legacy section, never reached by decode. See PROVENANCE.md. */
static inline void avpriv_request_sample(void *avcl, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    av_log(avcl, AV_LOG_ERROR, "Sample not found: ");
    av_vlog(avcl, AV_LOG_ERROR, fmt, ap);
    va_end(ap);
}

#endif /* AT3P_LIBAVCODEC_INTERNAL_H */
