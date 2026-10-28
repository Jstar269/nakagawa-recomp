/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * tlzrc.c: LZRC decodeer
 *   based on benhur's code, rewrite by tpu
 *
 * Copyright (c) 2012- PPSSPP Project.
 *
 * Ported from Core/FileSystems/tlzrc.cpp of PPSSPP
 * (https://github.com/hrydgard/ppsspp), revision
 * b4386554795949491a26e51b7d74082bb9b2470f.  Modified for this project:
 * C99 interface with const input and size_t lengths (see nk_psp_lzrc.c).
 */

#ifndef NK_PSP_LZRC_H
#define NK_PSP_LZRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Largest stream either side may be (the decoder keeps int offsets). */
#define NK_PSP_LZRC_MAX_LEN (64u * 1024u * 1024u)

/*
 * Decode one LZRC stream (the compression PSN UMD images use for their
 * blocks) into out.  Returns the number of bytes written, at most out_len,
 * or -1 when the stream is malformed, runs past its input, would write past
 * out_len, or refers back before the start of the output.  Never reads
 * outside in[0, in_len) or writes outside out[0, out_len).
 */
int nk_psp_lzrc_decompress(uint8_t *out, size_t out_len,
                           const uint8_t *in, size_t in_len);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_LZRC_H */
