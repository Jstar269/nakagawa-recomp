/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored minimal stand-in for FFmpeg n4.4 libavutil/avutil.h
 * (PR-A).
 *
 * Upstream avutil.h aggregates the whole libavutil public API (pixfmt,
 * rational, ...). The imported decoder subset only needs the declarations
 * below plus the subset headers it re-includes. See
 * src/rt/atrac3p/PROVENANCE.md.
 */

#ifndef AT3P_LIBAVUTIL_AVUTIL_H
#define AT3P_LIBAVUTIL_AVUTIL_H

#include "attributes.h"
#include "macros.h"
#include "version.h"
#include "error.h"
#include "common.h"

struct AVClass;

#endif /* AT3P_LIBAVUTIL_AVUTIL_H */
