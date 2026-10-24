// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Nakagawa-authored build configuration for the vendored FFmpeg n4.4 H.264 decoder subset.
 *
 * This replaces the configure-generated config.h of FFmpeg n4.4. The subset is built from the
 * generic C paths only: no assembly, no inline assembly, no threads, no hardware acceleration,
 * and no operating-system services beyond libc. Every symbol the imported sources reference is
 * listed, so a missing definition is a compile error instead of a silent zero.
 * See src/rt/h264_ffmpeg_sub/PROVENANCE.md.
 */
#ifndef NK_H264_SUB_CONFIG_H
#define NK_H264_SUB_CONFIG_H

#include "libavutil/avconfig.h"

#define FFMPEG_CONFIGURATION "nakagawa-h264-subset"
#define FFMPEG_LICENSE "LGPL version 2.1 or later"
#define CONFIG_THIS_YEAR 2021
#define av_restrict restrict
#define EXTERN_PREFIX ""
#define CC_IDENT "nakagawa"
#define OS_NAME nakagawa
#define FFMPEG_DATADIR "."
#define AVCONV_DATADIR "."

/* ---- architecture: generic C only ---------------------------------------------------------- */
#define ARCH_AARCH64 0
#define ARCH_ALPHA 0
#define ARCH_ARM 0
#define ARCH_AVR32 0
#define ARCH_IA64 0
#define ARCH_MIPS 0
#define ARCH_PPC 0
#define ARCH_SH4 0
#define ARCH_TOMI 0
#define ARCH_X86 0
#define ARCH_X86_32 0
#define HAVE_AVX 0
#define HAVE_AVX512 0
#define HAVE_INLINE_ASM 0
#define HAVE_MIPSFPU 0
#define HAVE_MMX 0
#define HAVE_MMXEXT 0
#define HAVE_MMX2 HAVE_MMXEXT /* libavutil/utils.c asserts HAVE_MMX2 == HAVE_MMXEXT */
#define HAVE_MMX_INLINE 0
#define HAVE_NEON 0
#define HAVE_SIMD_ALIGN_16 0
#define HAVE_SIMD_ALIGN_32 0
#define HAVE_SIMD_ALIGN_64 0
#define HAVE_SYMVER_ASM_LABEL 0
#define HAVE_SYMVER_GNU_ASM 0
#define HAVE_FAST_CLZ 0
#define HAVE_FAST_UNALIGNED 0
#define HAVE_LOCAL_ALIGNED 1
#define HAVE_PRAGMA_DEPRECATED 1

#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
#define HAVE_BIGENDIAN 1
#else
#define HAVE_BIGENDIAN 0
#endif
#if defined(__LP64__) || defined(_WIN64)
#define HAVE_FAST_64BIT 1
#else
#define HAVE_FAST_64BIT 0
#endif

/* ---- decoder configuration ----------------------------------------------------------------- */
#define CONFIG_ERROR_RESILIENCE 1
#define CONFIG_GRAY 0
#define CONFIG_GPL 0
#define CONFIG_SMALL 0
#define CONFIG_MEMORY_POISONING 0
#define CONFIG_SAFE_BITSTREAM_READER 1 /* upstream default: the bounds-checked bit reader */
#define CONFIG_SHARED 0
#define CONFIG_ICONV 0
#define CONFIG_OSSFUZZ 0
#define CONFIG_LINUX_PERF 0
#define CONFIG_VALGRIND_BACKTRACE 0
#define CONFIG_FRAME_THREAD_ENCODER 0
#define CONFIG_MPEGVIDEO 0
#define CONFIG_DWT 0
#define CONFIG_SNOW_DECODER 0
#define CONFIG_SNOW_ENCODER 0
#define CONFIG_XVMC 0
#define CONFIG_H264_D3D11VA_HWACCEL 0
#define CONFIG_H264_D3D11VA2_HWACCEL 0
#define CONFIG_H264_DXVA2_HWACCEL 0
#define CONFIG_H264_NVDEC_HWACCEL 0
#define CONFIG_H264_VAAPI_HWACCEL 0
#define CONFIG_H264_VDPAU_HWACCEL 0
#define CONFIG_H264_VIDEOTOOLBOX_HWACCEL 0
#define CONFIG_CUDA 0
#define CONFIG_D3D11VA 0
#define CONFIG_DXVA2 0
#define CONFIG_LIBDRM 0
#define CONFIG_MEDIACODEC 0
#define CONFIG_OPENCL 0
#define CONFIG_QSV 0
#define CONFIG_VAAPI 0
#define CONFIG_VDPAU 0
#define CONFIG_VIDEOTOOLBOX 0
#define CONFIG_VULKAN 0

/* ---- threads and operating-system services: none ------------------------------------------- */
#define HAVE_THREADS 0
#define HAVE_PTHREADS 0
#define HAVE_OS2THREADS 0
#define HAVE_W32THREADS 0
#define HAVE_BCRYPT 0
#define HAVE_ARC4RANDOM 0
#define HAVE_GETHRTIME 0
#define HAVE_GETSTDHANDLE 0
#define HAVE_GETSYSTEMTIMEASFILETIME 0
#define HAVE_SETCONSOLETEXTATTRIBUTE 0
#define HAVE_SLEEP 0
#define HAVE_MACH_ABSOLUTE_TIME 0
#define HAVE_VALGRIND_VALGRIND_H 0
#define HAVE_LIBC_MSVCRT 0
#define HAVE_NANOSLEEP 0
#define HAVE_USLEEP 0
#define HAVE_FCNTL 0
#define HAVE_ISATTY 0
#define HAVE_MKSTEMP 0
#define HAVE_STRERROR_R 0
#define HAVE_CLOCK_GETTIME 0
#define HAVE_GETTIMEOFDAY 0
#define HAVE_WINDOWS_H 0
#define HAVE_IO_H 0
#define HAVE_DOS_PATHS 0

#if defined(_WIN32)
#define HAVE_ALIGNED_MALLOC 1
#define HAVE_POSIX_MEMALIGN 0
#define HAVE_MEMALIGN 0
#define HAVE_MALLOC_H 1
#define HAVE_UNISTD_H 0
#define HAVE_GMTIME_R 0
#define HAVE_LOCALTIME_R 0
#else
#define HAVE_ALIGNED_MALLOC 0
#define HAVE_POSIX_MEMALIGN 1
#define HAVE_MEMALIGN 0
#define HAVE_MALLOC_H 0
#define HAVE_UNISTD_H 1
#define HAVE_GMTIME_R 1
#define HAVE_LOCALTIME_R 1
#endif

/* ---- C99 math: every supported host provides these ----------------------------------------- */
#define HAVE_ATANF 1
#define HAVE_ATAN2F 1
#define HAVE_CBRT 1
#define HAVE_CBRTF 1
#define HAVE_COPYSIGN 1
#define HAVE_COSF 1
#define HAVE_ERF 1
#define HAVE_EXP2 1
#define HAVE_EXP2F 1
#define HAVE_EXPF 1
#define HAVE_HYPOT 1
#define HAVE_ISFINITE 1
#define HAVE_ISINF 1
#define HAVE_ISNAN 1
#define HAVE_LDEXPF 1
#define HAVE_LLRINT 1
#define HAVE_LLRINTF 1
#define HAVE_LOG2 1
#define HAVE_LOG2F 1
#define HAVE_LOG10F 1
#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define HAVE_POWF 1
#define HAVE_RINT 1
#define HAVE_ROUND 1
#define HAVE_ROUNDF 1
#define HAVE_SINF 1
#define HAVE_TRUNC 1
#define HAVE_TRUNCF 1

#endif /* NK_H264_SUB_CONFIG_H */
