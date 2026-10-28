/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  amctrl.h  -- Reverse engineering of amctrl.prx
 *               written by tpu.
 *
 * Ported from ext/libkirk/amctrl.h of PPSSPP
 * (https://github.com/hrydgard/ppsspp), revision
 * b4386554795949491a26e51b7d74082bb9b2470f.  The upstream file carries no
 * licence notice of its own; PPSSPP ships it under its GPL-2.0-or-later
 * licence beside KIRK-ENGINE (GPL-3.0-or-later), and this port is conveyed
 * as GPL-3.0-or-later.
 *
 * Modified for this project: the DRM BB MAC and cipher subset a PSN UMD
 * image (NPUMDIMG) needs, decrypt direction only; every call takes an
 * NkPspCtx so the keyvault rows and amctrl constants come from the KeyStore
 * and a missing entry fails closed by name; results are the boundary's
 * NK_PSP_* codes.  MAC type 2 and cipher type 2 use the console's fuse ID
 * (KIRK commands 5 and 8) and are refused as CONTEXT_REQUIRED; nothing runs
 * them with a default.  The PGD and fixed-key (sceNpDrmGetFixedKey)
 * functions and the encrypt direction are not ported.
 */

#ifndef NK_PSP_AMCTRL_H
#define NK_PSP_AMCTRL_H

#include "nk_psp_crypto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int type;
	u8 key[16];
	u8 pad[16];
	int pad_size;
} MAC_KEY;

typedef struct
{
	u32 type;
	u32 seed;
	u8 key[16];
} CIPHER_KEY;

/* MAC type 3 (fixed key, MAC encrypted again) is the one enabled; type 2
 * needs the console's fuse ID and fails CONTEXT_REQUIRED. */
int sceDrmBBMacInit(NkPspCtx *ctx, MAC_KEY *mkey, int type);
int sceDrmBBMacUpdate(NkPspCtx *ctx, MAC_KEY *mkey, const u8 *buf, int size);
int sceDrmBBMacFinal(NkPspCtx *ctx, MAC_KEY *mkey, u8 *buf, const u8 *vkey);
/* Derive the version key a type-3 BB MAC was made with. */
int bbmac_getkey(NkPspCtx *ctx, MAC_KEY *mkey, const u8 *bbmac, u8 *vkey);

/* Cipher type 1 (fixed key), mode 2 (decrypt) is the one enabled; type 2
 * needs the console's fuse ID (CONTEXT_REQUIRED), and mode 1 (encrypt,
 * which draws a fresh key from the KIRK PRNG) is not part of the boundary. */
int sceDrmBBCipherInit(NkPspCtx *ctx, CIPHER_KEY *ckey, int type, int mode,
                       const u8 *header_key, const u8 *version_key, u32 seed);
/* XOR the keystream into data (size bytes, any length up to INT_MAX). */
int sceDrmBBCipherUpdate(NkPspCtx *ctx, CIPHER_KEY *ckey, u8 *data, int size);
int sceDrmBBCipherFinal(CIPHER_KEY *ckey);

#ifdef __cplusplus
}
#endif

#endif /* NK_PSP_AMCTRL_H */
