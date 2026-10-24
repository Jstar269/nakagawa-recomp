// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Nakagawa-authored replacement for the configure-generated codec_list.c: H.264 decoder only. */
static const AVCodec * const codec_list[] = {
    &ff_h264_decoder,
    NULL };
