/*
// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
 ** Nakagawa-authored standalone-build configuration (PR-A).
 *
 * This header is OUR replacement for the configure-generated
 * libavutil/config.h of FFmpeg n4.4. All features are disabled so the
 * imported decoder subset compiles through the generic C paths only.
 * See src/rt/atrac3p/PROVENANCE.md.
 */

#ifndef AT3P_LIBAVUTIL_CONFIG_H
#define AT3P_LIBAVUTIL_CONFIG_H

/* av_restrict is emitted by configure into libavutil/avconfig.h (configure
 * line 7592 of n4.4). Imported headers such as libavutil/float_dsp.h use it
 * at parse time but only include "config.h", so this standalone subset
 * pulls avconfig.h in here to guarantee ordering; see PROVENANCE.md. */
#include "avconfig.h"

#define CONFIG_HARDCODED_TABLES 0
#define CONFIG_SMALL 0
/* configure emits every option symbol; memory poisoning is off by default. */
#define CONFIG_MEMORY_POISONING 0

#define ARCH_AARCH64 0
#define ARCH_ALPHA 0
#define ARCH_ARM 0
#define ARCH_AVR32 0
#define ARCH_AVR 0
#define ARCH_BFIN 0
#define ARCH_CRIS 0
#define ARCH_LOONGARCH 0
#define ARCH_M68K 0
#define ARCH_MIPS 0
#define ARCH_MIPS64 0
#define ARCH_PARISC 0
#define ARCH_PPC 0
#define ARCH_PPC64 0
#define ARCH_S390 0
#define ARCH_SH4 0
#define ARCH_SPARC 0
#define ARCH_SPARC64 0
#define ARCH_TILEGX 0
#define ARCH_TILEPRO 0
#define ARCH_TOMI 0
#define ARCH_X86 0
#define ARCH_X86_32 0
#define ARCH_X86_64 0

#define HAVE_AVX 0
#define HAVE_AVX2 0
#define HAVE_AVX512 0
#define HAVE_BIGENDIAN 0
#define HAVE_FAST_UNALIGNED 0
#define HAVE_INLINE_ASM 0
#define HAVE_INTRINSICS 0
#define HAVE_MMX 0
#define HAVE_MMXEXT 0
#define HAVE_SSE 0
#define HAVE_SSE2 0
#define HAVE_SSE3 0
#define HAVE_SSE4 0
#define HAVE_SSSE3 0
#define HAVE_X86ASM 0

#define HAVE_ALTIVEC 0
#define HAVE_VSX 0
#define HAVE_MIPSFPU 0
#define HAVE_MMI 0

#if defined(_WIN32)
#define HAVE_ALIGNED_MALLOC 1
#define HAVE_POSIX_MEMALIGN 0
#define HAVE_MALLOC_H 1
#else
#define HAVE_ALIGNED_MALLOC 0
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_MALLOC_H 0
#endif
#define HAVE_MEMALIGN 0

/* FF_MEMORY_POISON is used by the memory-poisoning branch of
 * libavutil/mem.c:334. Upstream n4.4 defines it only in
 * libavutil/internal.h (line 88), which mem.c does not include - an
 * upstream include-graph quirk that this standalone subset resolves here
 * (mem.c remains byte-identical; the branch is dead because
 * CONFIG_MEMORY_POISONING is 0). Upstream later fixed this by moving the
 * constant into mem.c (0f78b26e9c). See PROVENANCE.md. */
#define FF_MEMORY_POISON 0x2a

#endif /* AT3P_LIBAVUTIL_CONFIG_H */
