/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Ported from libkirk/AES.c and AES.h of John-K/pspdecrypt
 * (https://github.com/John-K/pspdecrypt), commit
 * c156627db7634d395c380c0a9589130f603307fc.
 * The pspdecrypt repository's LICENSE.TXT is the GNU GPL version 3 with no
 * later-version grant, so this port is labelled GPL-3.0-only.
 * The underlying Rijndael implementation carries the notices in
 * nk_psp_aes.c (OpenBSD rijndael.c and rijndael-alg-fst.c 3.0, public
 * domain).  Modified for this project: include paths, header guards and
 * API surface only -- no key material is present in this file.
 */

#ifndef NK_PSP_AES_H
#define NK_PSP_AES_H

#include "nk_psp_crypto.h"

#define AES_KEY_LEN_128	(128)
#define AES_KEY_LEN_192	(192)
#define AES_KEY_LEN_256	(256)

#define AES_BUFFER_SIZE (16)

#define AES_MAXKEYBITS	(256)
#define AES_MAXKEYBYTES	(AES_MAXKEYBITS/8)
/* for 256-bit keys, fewer for less */
#define AES_MAXROUNDS	14
#define pwuAESContextBuffer rijndael_ctx

/*  The structure for key information */
typedef struct 
{
	int	enc_only;		/* context contains only encrypt schedule */
	int	Nr;			/* key-length-dependent number of rounds */
	u32	ek[4*(AES_MAXROUNDS + 1)];	/* encrypt key schedule */
	u32	dk[4*(AES_MAXROUNDS + 1)];	/* decrypt key schedule */
} rijndael_ctx;

typedef struct 
{
	int	enc_only;		/* context contains only encrypt schedule */
	int	Nr;			/* key-length-dependent number of rounds */
	u32	ek[4*(AES_MAXROUNDS + 1)];	/* encrypt key schedule */
	u32	dk[4*(AES_MAXROUNDS + 1)];	/* decrypt key schedule */
} AES_ctx;

int rijndael_set_key(rijndael_ctx *, const u8 *, int);
int	rijndael_set_key_enc_only(rijndael_ctx *, const u8 *, int);
void rijndael_decrypt(rijndael_ctx *, const u8 *, u8 *);
void rijndael_encrypt(rijndael_ctx *, const u8 *, u8 *);

int AES_set_key(AES_ctx *ctx, const u8 *key, int bits);
void AES_encrypt(AES_ctx *ctx, const u8 *src, u8 *dst);
void AES_decrypt(AES_ctx *ctx, const u8 *src, u8 *dst);
void AES_cbc_encrypt(AES_ctx *ctx, const u8 *src, u8 *dst, int size);
void AES_cbc_decrypt(AES_ctx *ctx, const u8 *src, u8 *dst, int size);
void AES_CMAC(AES_ctx *ctx, unsigned char *input, int length, unsigned char *mac);

int	rijndaelKeySetupEnc(unsigned int [], const unsigned char [], int);
int	rijndaelKeySetupDec(unsigned int [], const unsigned char [], int);
void rijndaelEncrypt(const unsigned int [], int, const unsigned char [16],
            unsigned char [16]);

#endif /* NK_PSP_AES_H */
