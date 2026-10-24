// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// VFPU cell macros for the oracle's VFPU batch cells (today probe_hle_issues.c's
// hle-issues-batch): whole-register-file image load/store, VFPU_CC preset and readback,
// the two vpfxs forms, one cell function per instruction word, and the RCX (random
// unit control) readback and write. Encodings are emitted as words so no assembler
// mnemonic decides them. A later VFPU batch case includes this header rather than
// repeating the macros.
#ifndef NK_HLE_VFPU_CELLS_H
#define NK_HLE_VFPU_CELLS_H

/* The whole VFPU register file moves as a 128-word image, word m*16 + c*4 + r holding
   S<m><c><r> (column quad C<m><c>0 is four consecutive words). Each cell loads the
   image (32 lv.q), presets VFPU_CC with mtvc, reads it back (cc[0]), optionally issues
   one vpfxs, executes the exact instruction word, stores the image again (32 sv.q) and
   reads VFPU_CC (cc[1]). Nothing else runs between the prefix and the word. mtvc/mfvc
   are emitted as words (COP2 rs=7/rs=3, imm 128 + control index; VFPU_CC is control 3)
   with $t0/$t1 fixed, so no assembler mnemonic decides their encoding. */
#define TN_IMAGE_WORDS 128u
#define TN_CC_PRESET 0x15u
#define TN_SENTINEL 0x7a5a5a5au

#define TN_LOAD_ALL \
    "lv.q C000, 0(%[in])\n\tlv.q C010, 16(%[in])\n\tlv.q C020, 32(%[in])\n\tlv.q C030, 48(%[in])\n\t"       \
    "lv.q C100, 64(%[in])\n\tlv.q C110, 80(%[in])\n\tlv.q C120, 96(%[in])\n\tlv.q C130, 112(%[in])\n\t"     \
    "lv.q C200, 128(%[in])\n\tlv.q C210, 144(%[in])\n\tlv.q C220, 160(%[in])\n\tlv.q C230, 176(%[in])\n\t" \
    "lv.q C300, 192(%[in])\n\tlv.q C310, 208(%[in])\n\tlv.q C320, 224(%[in])\n\tlv.q C330, 240(%[in])\n\t" \
    "lv.q C400, 256(%[in])\n\tlv.q C410, 272(%[in])\n\tlv.q C420, 288(%[in])\n\tlv.q C430, 304(%[in])\n\t" \
    "lv.q C500, 320(%[in])\n\tlv.q C510, 336(%[in])\n\tlv.q C520, 352(%[in])\n\tlv.q C530, 368(%[in])\n\t" \
    "lv.q C600, 384(%[in])\n\tlv.q C610, 400(%[in])\n\tlv.q C620, 416(%[in])\n\tlv.q C630, 432(%[in])\n\t" \
    "lv.q C700, 448(%[in])\n\tlv.q C710, 464(%[in])\n\tlv.q C720, 480(%[in])\n\tlv.q C730, 496(%[in])\n\t"
#define TN_STORE_ALL \
    "sv.q C000, 0(%[out])\n\tsv.q C010, 16(%[out])\n\tsv.q C020, 32(%[out])\n\tsv.q C030, 48(%[out])\n\t"       \
    "sv.q C100, 64(%[out])\n\tsv.q C110, 80(%[out])\n\tsv.q C120, 96(%[out])\n\tsv.q C130, 112(%[out])\n\t"     \
    "sv.q C200, 128(%[out])\n\tsv.q C210, 144(%[out])\n\tsv.q C220, 160(%[out])\n\tsv.q C230, 176(%[out])\n\t" \
    "sv.q C300, 192(%[out])\n\tsv.q C310, 208(%[out])\n\tsv.q C320, 224(%[out])\n\tsv.q C330, 240(%[out])\n\t" \
    "sv.q C400, 256(%[out])\n\tsv.q C410, 272(%[out])\n\tsv.q C420, 288(%[out])\n\tsv.q C430, 304(%[out])\n\t" \
    "sv.q C500, 320(%[out])\n\tsv.q C510, 336(%[out])\n\tsv.q C520, 352(%[out])\n\tsv.q C530, 368(%[out])\n\t" \
    "sv.q C600, 384(%[out])\n\tsv.q C610, 400(%[out])\n\tsv.q C620, 416(%[out])\n\tsv.q C630, 432(%[out])\n\t" \
    "sv.q C700, 448(%[out])\n\tsv.q C710, 464(%[out])\n\tsv.q C720, 480(%[out])\n\tsv.q C730, 496(%[out])\n\t"

/* Two vpfxs forms: P1 negates every lane with the identity swizzle (xyzw);
   P2 reverses the lanes (wzyx) and negates lanes 0 and 2. */
#define TN_PFX_NONE ""
#define TN_PFX_P1 ".word 0xdc0f00e4\n\t"
#define TN_PFX_P2 ".word 0xdc05001b\n\t"
#define TN_PFXW_NONE 0u
#define TN_PFXW_P1 0xdc0f00e4u
#define TN_PFXW_P2 0xdc05001bu

#define TN_CELL_FN(fname, pfx, word)                                                   \
    static void fname(const uint32_t *in, uint32_t *out, uint32_t *cc)                 \
    {                                                                                  \
        __asm__ volatile(                                                              \
            "move $t0, %[pre]\n\t"                                                     \
            ".word 0x48e80083\n\t" /* mtvc $t0, VFPU_CC */                             \
            TN_LOAD_ALL                                                                \
            ".word 0x48690083\n\t" /* mfvc $t1, VFPU_CC */                             \
            "sw $t1, 0(%[cc])\n\t"                                                     \
            pfx                                                                        \
            ".word " word "\n\t"                                                       \
            TN_STORE_ALL                                                               \
            ".word 0x48690083\n\t"                                                     \
            "sw $t1, 4(%[cc])\n\t"                                                     \
            :                                                                          \
            : [in] "r"(in), [out] "r"(out), [cc] "r"(cc), [pre] "r"(TN_CC_PRESET)      \
            : "t0", "t1", "memory");                                                   \
    }

#define TN_CELL_SET(stem, word)                 \
    TN_CELL_FN(stem##_none, TN_PFX_NONE, word)  \
    TN_CELL_FN(stem##_p1, TN_PFX_P1, word)      \
    TN_CELL_FN(stem##_p2, TN_PFX_P2, word)

/* Reads the eight RCX control registers (VFPU controls 8-15) into dst[0..7] with mfvc
   ($t1 clobbered). */
#define TN_RCX_READ(dst)                                                          \
    ".word 0x48690088\n\tsw $t1, 0(" dst ")\n\t.word 0x48690089\n\tsw $t1, 4(" dst ")\n\t"   \
    ".word 0x4869008a\n\tsw $t1, 8(" dst ")\n\t.word 0x4869008b\n\tsw $t1, 12(" dst ")\n\t"  \
    ".word 0x4869008c\n\tsw $t1, 16(" dst ")\n\t.word 0x4869008d\n\tsw $t1, 20(" dst ")\n\t" \
    ".word 0x4869008e\n\tsw $t1, 24(" dst ")\n\t.word 0x4869008f\n\tsw $t1, 28(" dst ")\n\t"

/* Writes RCX0-7 (VFPU controls 8-15) from src[0..7] with mtvc ($t1 = COP2 rt 9, imm 128 + control index;
   $t1 clobbered). */
#define TN_RCX_WRITE(src)                                                                         \
    "lw $t1, 0(" src ")\n\t.word 0x48e90088\n\tlw $t1, 4(" src ")\n\t.word 0x48e90089\n\t"   \
    "lw $t1, 8(" src ")\n\t.word 0x48e9008a\n\tlw $t1, 12(" src ")\n\t.word 0x48e9008b\n\t"  \
    "lw $t1, 16(" src ")\n\t.word 0x48e9008c\n\tlw $t1, 20(" src ")\n\t.word 0x48e9008d\n\t" \
    "lw $t1, 24(" src ")\n\t.word 0x48e9008e\n\tlw $t1, 28(" src ")\n\t.word 0x48e9008f\n\t"

#endif
