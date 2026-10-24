// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Nakagawa-authored replacement for the configure-generated bsf_list.c: null filter only. */
static const AVBitStreamFilter * const bitstream_filters[] = {
    &ff_null_bsf,
    NULL };
