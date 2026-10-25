// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Symbol namespace for the vendored FFmpeg n4.4 H.264 subset.
 *
 * The runtime also links the vendored ATRAC3/ATRAC3+ subset (src/rt/atrac3p/), which carries its
 * own copies of a few libavutil/libavcodec files at the same pin. Both subsets are statically
 * linked into one program, so the global symbols they share would collide. This header is
 * force-included (-include) into every translation unit of this subset and renames exactly those
 * shared symbols, which keeps the imported files byte-identical to upstream. The list is the
 * intersection of the two subsets' defined global symbols; the link of the runtime (or of
 * psmf-media-selftest) fails on any collision this list misses.
 * See src/rt/h264_ffmpeg_sub/PROVENANCE.md.
 */
#ifndef NK_H264_SYMBOLS_H
#define NK_H264_SYMBOLS_H

#define av_calloc nkh264_av_calloc
#define av_dynarray2_add nkh264_av_dynarray2_add
#define av_dynarray_add nkh264_av_dynarray_add
#define av_dynarray_add_nofree nkh264_av_dynarray_add_nofree
#define av_fast_malloc nkh264_av_fast_malloc
#define av_fast_mallocz nkh264_av_fast_mallocz
#define av_fast_realloc nkh264_av_fast_realloc
#define av_free nkh264_av_free
#define av_freep nkh264_av_freep
#define av_log nkh264_av_log
#define av_malloc nkh264_av_malloc
#define av_malloc_array nkh264_av_malloc_array
#define av_mallocz nkh264_av_mallocz
#define av_mallocz_array nkh264_av_mallocz_array
#define av_max_alloc nkh264_av_max_alloc
#define av_memcpy_backptr nkh264_av_memcpy_backptr
#define av_memdup nkh264_av_memdup
#define av_realloc nkh264_av_realloc
#define av_realloc_array nkh264_av_realloc_array
#define av_realloc_f nkh264_av_realloc_f
#define av_reallocp nkh264_av_reallocp
#define av_reallocp_array nkh264_av_reallocp_array
#define av_strdup nkh264_av_strdup
#define av_strndup nkh264_av_strndup
#define av_vlog nkh264_av_vlog
#define avpriv_align_put_bits nkh264_avpriv_align_put_bits
#define avpriv_copy_bits nkh264_avpriv_copy_bits
#define ff_copy_bits nkh264_ff_copy_bits
#define ff_free_vlc nkh264_ff_free_vlc
#define ff_init_vlc_from_lengths nkh264_ff_init_vlc_from_lengths
#define ff_init_vlc_sparse nkh264_ff_init_vlc_sparse
#define ff_log2_run nkh264_ff_log2_run
#define ff_log2_tab nkh264_ff_log2_tab
#define ff_put_string nkh264_ff_put_string
#define ff_reverse nkh264_ff_reverse

#endif /* NK_H264_SYMBOLS_H */
