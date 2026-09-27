/* SPDX-License-Identifier: GPL-3.0-only
 * Copyright (C) 2026 the Nakagawa Recomp authors
 *
 * Ported from libkirk/SHA1.c and SHA1.h of John-K/pspdecrypt
 * (https://github.com/John-K/pspdecrypt), commit
 * c156627db7634d395c380c0a9589130f603307fc.
 * The pspdecrypt repository's LICENSE.TXT is the GNU GPL version 3 with no
 * later-version grant, so this port is labelled GPL-3.0-only.
 * Original implementation credits (kept verbatim in nk_psp_sha1.c):
 * November 2000, David Ireland, DI Management Services Pty Limited;
 * adapted from the Python Cryptography Toolkit (A.M. Kuchling 1995) and
 * SHA code originally posted by Peter Gutmann.
 * Modified for this project: header guards and the include path only.
 */

#ifndef NK_PSP_SHA1_H
#define NK_PSP_SHA1_H

/* POINTER defines a generic pointer type */
typedef unsigned char *POINTER;

/* UINT4 defines a four byte word */
typedef unsigned int UINT4;

/* BYTE defines a unsigned character */
typedef unsigned char BYTE;

#ifndef TRUE
  #define FALSE	0
  #define TRUE	( !FALSE )
#endif /* TRUE */

#endif /* end _GLOBAL_H_ */

/* sha.h */

#ifndef NK_PSP_SHA_H_
#define NK_PSP_SHA_H_ 1

/* #include "global.h" */

/* The structure for storing SHS info */

typedef struct 
{
	UINT4 digest[ 5 ];            /* Message digest */
	UINT4 countLo, countHi;       /* 64-bit bit count */
	UINT4 data[ 16 ];             /* SHS data buffer */
	int Endianness;
} SHA_CTX;

/* Message digest functions */

void SHAInit(SHA_CTX *);
void SHAUpdate(SHA_CTX *, BYTE *buffer, int count);
void SHAFinal(BYTE *output, SHA_CTX *);

#endif /* end _SHA_H_ */

/* endian.h */

#ifndef NK_PSP_ENDIAN_H_
#define NK_PSP_ENDIAN_H_ 1

void endianTest(int *endianness);

#endif /* end _ENDIAN_H_ */
