/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Ported from PrxDecrypter.h of John-K/pspdecrypt
 * (https://github.com/John-K/pspdecrypt), commit
 * c156627db7634d395c380c0a9589130f603307fc.  The PSP_Header layout
 * originates in PPSSPP's Core/ELF/PrxDecrypter.h:
 * Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
 * The pspdecrypt repository's LICENSE.TXT is the GNU GPL version 3 with no
 * later-version grant, so this port is labelled GPL-3.0-only.
 * Modified for this project: C99 spelling, packed attribute kept
 * portable, and the API threads an NkPspCtx.  No key material here.
 */

#ifndef NK_PSP_PRX_H
#define NK_PSP_PRX_H

#include "nk_psp_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)
typedef struct {
    u32 signature;         /* 0x00 "~PSP" */
    u16 attribute;         /* 0x04 modinfo */
    u16 comp_attribute;    /* 0x06 bit0 = compressed payload */
    u8  module_ver_lo;     /* 0x08 */
    u8  module_ver_hi;     /* 0x09 */
    char modname[28];      /* 0x0A */
    u8  version;           /* 0x26 */
    u8  nsegments;         /* 0x27 */
    u32 elf_size;          /* 0x28 */
    u32 psp_size;          /* 0x2C total container size */
    u32 entry;             /* 0x30 */
    u32 modinfo_offset;    /* 0x34 */
    s32 bss_size;          /* 0x38 */
    u16 seg_align[4];      /* 0x3C */
    u32 seg_address[4];    /* 0x44 */
    s32 seg_size[4];       /* 0x54 */
    u32 reserved[5];       /* 0x64 */
    u32 devkitversion;     /* 0x78 */
    u32 decrypt_mode;      /* 0x7C */
    u8  key_data0[0x30];   /* 0x80 (scrambled KIRK material for type 2/5/6) */
    s32 comp_size;         /* 0xB0 (= KIRK data_size) */
    s32 _80;               /* 0xB4 (= KIRK data_offset) */
    s32 reserved2[2];      /* 0xB8 */
    u8  key_data1[0x10];   /* 0xC0 */
    u32 tag;               /* 0xD0 decryption tag */
    u8  scheck[0x58];      /* 0xD4 */
    u32 key_data2;         /* 0x12C */
    u32 oe_tag;            /* 0x130 */
    u8  key_data3[0x1C];   /* 0x134 */
} PSP_Header;
#pragma pack(pop)

/*
 * Decrypt an encrypted ~PSP/PRX container in place-ish fashion: reads
 * ``size`` bytes from inbuf, writes the decrypted payload (not the
 * 0x150-byte header) to outbuf, and returns the payload length (> 0).
 * On failure returns a negative NK_PSP_ERR_* code with the context filled
 * in (missing key entry name, integrity/format message).
 *
 * ``seed`` is an optional 16-byte bonus seed used by a small set of
 * NPDRM-era tag recipes; the disc/CLI boundary passes NULL.
 *
 * Every tag recipe (key, code/keyvault slot, seed) comes from the
 * KeyStore entry ``prx.tag.0xXXXXXXXX``; no tags or keys are compiled in.
 */
int pspDecryptPRX(NkPspCtx *ctx, const u8 *inbuf, u8 *outbuf,
                  u32 size, const u8 *seed);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_PRX_H */
