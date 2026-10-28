/* SPDX-License-Identifier: GPL-3.0-or-later
 *
 *  amctrl.c  -- Reverse engineering of amctrl.prx
 *               written by tpu.
 *
 * Ported from ext/libkirk/amctrl.c of PPSSPP
 * (https://github.com/hrydgard/ppsspp), revision
 * b4386554795949491a26e51b7d74082bb9b2470f.  The upstream file carries no
 * licence notice of its own; PPSSPP ships it under its GPL-2.0-or-later
 * licence beside KIRK-ENGINE (GPL-3.0-or-later), and this port is conveyed
 * as GPL-3.0-or-later.
 *
 * Modified for this project (see src/core/PROVENANCE.md):
 *   - the DRM BB MAC (type 3) and cipher (type 1, decrypt) subset a PSN UMD
 *     image needs: sceDrmBBMacInit/Update/Final, bbmac_getkey and
 *     sceDrmBBCipherInit/Update/Final.  sceDrmBBMacFinal2, the fixed-key
 *     and PGD functions and the encrypt direction are not ported;
 *   - every call takes an NkPspCtx: keyvault rows 0x38, 0x39 and 0x63 go
 *     through the engine's kirk4/kirk7 passes and the loc_1CD4, loc_1CE4
 *     and loc_1CF4 constants are KeyStore entries (amctrl.loc_*), so the
 *     tables live in nk_psp_builtin_keys.c and a missing one fails closed by
 *     name;
 *   - the fuse-ID types (MAC type 2, cipher type 2: KIRK commands 5 and 8)
 *     are refused as CONTEXT_REQUIRED instead of running with a default
 *     fuse ID;
 *   - the work buffers are local and bounded; a cipher run whose length is
 *     not a multiple of 16 derives its last keystream block in full and uses
 *     only the bytes it needs, as upstream's oversized KIRK buffer did;
 *   - a MAC update that leaves only the previous full pad block chains that
 *     block instead of dropping it, and results are NK_PSP_* codes.
 */

#include <limits.h>
#include <string.h>

#include "nk_psp_amctrl.h"
#include "nk_psp_kirk.h"

#define AMCTRL_CHUNK 0x0800
#define AMCTRL_MAC_CODE 0x38    /* MAC type 3 (and 1): fixed key */
#define AMCTRL_CIPHER_CODE 0x39 /* cipher type 1: fixed key */
#define AMCTRL_BBMAC_CODE 0x63  /* type-3 MAC wrap, cipher keystream */

/*************************************************************/

static void amctrl_put32le(u8 *p, u32 v)
{
	p[0] = (u8)v;
	p[1] = (u8)(v >> 8);
	p[2] = (u8)(v >> 16);
	p[3] = (u8)(v >> 24);
}

/* One 16-byte amctrl constant from the KeyStore (amctrl.loc_1CD4, ...). */
static int amctrl_const(NkPspCtx *ctx, const char *name, const u8 **out)
{
	size_t len = 0;
	int rc = nk_psp_need(ctx, name, out, &len);
	if (rc != NK_PSP_OK) return rc;
	if (len != 16) {
		return nk_psp_fail(ctx, NK_PSP_ERR_KEYFILE, name,
		                   "key entry %s must be 16 bytes", name);
	}
	return NK_PSP_OK;
}

static int amctrl_kirk_rc(NkPspCtx *ctx, int keyId)
{
	if (ctx->missing_entry[0] != '\0') return NK_PSP_ERR_MISSING_KEY;
	return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
	                   "the KIRK pass under keyvault row 0x%02X failed",
	                   (unsigned)keyId);
}

/* KIRK command 4 with no header: AES-CBC encrypt under a keyvault row. */
static int do_kirk4(NkPspCtx *ctx, u8 *out, const u8 *in, int size, int type)
{
	if (kirk4(ctx, out, in, (size_t)size, type) != KIRK_OPERATION_SUCCESS)
		return amctrl_kirk_rc(ctx, type);
	return NK_PSP_OK;
}

/* KIRK command 7 with no header: AES-CBC decrypt under a keyvault row. */
static int do_kirk7(NkPspCtx *ctx, u8 *out, const u8 *in, int size, int type)
{
	if (kirk7(ctx, out, in, (size_t)size, type) != KIRK_OPERATION_SUCCESS)
		return amctrl_kirk_rc(ctx, type);
	return NK_PSP_OK;
}

static int refuse_fuse_id(NkPspCtx *ctx, const char *what)
{
	return nk_psp_fail(ctx, NK_PSP_ERR_CONTEXT, NULL,
	                   "%s is bound to the console that installed it (its "
	                   "fuse ID); Nakagawa needs that value from your own "
	                   "PSP and never uses a default", what);
}

/*************************************************************/

// Called by sceDrmBBMacUpdate
// encrypt_buf
static int sub_158(NkPspCtx *ctx, u8 *buf, int size, u8 *key, int key_type)
{
	int i, retv;

	for(i=0; i<16; i++){
		buf[i] ^= key[i];
	}

	retv = do_kirk4(ctx, buf, buf, size, key_type);
	if(retv)
		return retv;

	// copy last 16 bytes to keys
	memcpy(key, buf+size-16, 16);

	return 0;
}


// type:
//      2: use fuse id (refused: CONTEXT_REQUIRED)
//      3: use fixed id
int sceDrmBBMacInit(NkPspCtx *ctx, MAC_KEY *mkey, int type)
{
	if (ctx == NULL || mkey == NULL) return NK_PSP_ERR_INTERNAL;
	memset(mkey, 0, sizeof(*mkey));
	if (type == 2)
		return refuse_fuse_id(ctx, "this DRM BB MAC (type 2)");
	if (type != 3) {
		return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
		                   "DRM BB MAC type %d is not part of the built-in "
		                   "decryption boundary", type);
	}
	mkey->type = type;
	mkey->pad_size = 0;

	return 0;
}

int sceDrmBBMacUpdate(NkPspCtx *ctx, MAC_KEY *mkey, const u8 *buf, int size)
{
	int retv = 0, ksize, p, type;
	u8 kbuf[AMCTRL_CHUNK];

	if (ctx == NULL || mkey == NULL || size < 0 || (buf == NULL && size != 0))
		return NK_PSP_ERR_INTERNAL;
	if (mkey->type != 3 || mkey->pad_size < 0 || mkey->pad_size > 16 ||
	    size > INT_MAX - 16) {
		return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
		                   "DRM BB MAC state is not initialised");
	}

	if(mkey->pad_size+size<=16){
		if (size > 0) memcpy(mkey->pad+mkey->pad_size, buf, (size_t)size);
		mkey->pad_size += size;
		retv = 0;
	}else{
		// copy pad data first
		memcpy(kbuf, mkey->pad, (size_t)mkey->pad_size);

		p = mkey->pad_size;

		mkey->pad_size += size;
		mkey->pad_size &= 0x0f;
		if(mkey->pad_size==0)
			mkey->pad_size = 16;

		size -= mkey->pad_size;
		// save last data to pad buf
		memcpy(mkey->pad, buf+size, (size_t)mkey->pad_size);

		type = AMCTRL_MAC_CODE;

		while(size+p > 0){
			ksize = (size+p>=AMCTRL_CHUNK)? AMCTRL_CHUNK : size+p;
			memcpy(kbuf+p, buf, (size_t)(ksize-p));
			retv = sub_158(ctx, kbuf, ksize, mkey->key, type);
			if(retv)
				goto _exit;
			size -= (ksize-p);
			buf += ksize-p;
			p = 0;
		}
	}

_exit:
	return retv;

}

static void amctrl_shift_left(u8 tmp[16])
{
	int i;
	u32 t0, v0, v1;

	// left shift tmp 1 bit
	t0 = (tmp[0]&0x80)? 0x87 : 0;
	for(i=0; i<15; i++){
		v1 = tmp[i+0];
		v0 = tmp[i+1];
		v1 <<= 1;
		v0 >>= 7;
		v0 |= v1;
		tmp[i+0] = (u8)v0;
	}
	v0 = tmp[15];
	v0 <<= 1;
	v0 ^= t0;
	tmp[15] = (u8)v0;
}

int sceDrmBBMacFinal(NkPspCtx *ctx, MAC_KEY *mkey, u8 *buf, const u8 *vkey)
{
	int i, retv, code;
	u8 kbuf[16], tmp[16], tmp1[16];
	const u8 *loc_1CD4 = NULL;

	if (ctx == NULL || mkey == NULL || buf == NULL) return NK_PSP_ERR_INTERNAL;
	if (mkey->type != 3 || mkey->pad_size < 0 || mkey->pad_size > 16) {
		return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
		                   "DRM BB MAC state is not initialised");
	}
	retv = amctrl_const(ctx, "amctrl.loc_1CD4", &loc_1CD4);
	if (retv)
		return retv;

	code = AMCTRL_MAC_CODE;

	memset(kbuf, 0, 16);
	retv = do_kirk4(ctx, kbuf, kbuf, 16, code);
	if(retv)
		goto _exit;
	memcpy(tmp, kbuf, 16);

	amctrl_shift_left(tmp);

	// padding remain data
	if(mkey->pad_size<16){
		amctrl_shift_left(tmp);

		mkey->pad[mkey->pad_size] = 0x80;
		if(mkey->pad_size+1<16)
			memset(mkey->pad+mkey->pad_size+1, 0, (size_t)(16-mkey->pad_size-1));
	}

	for(i=0; i<16; i++){
		mkey->pad[i] ^= tmp[i];
	}

	memcpy(kbuf, mkey->pad, 16);
	memcpy(tmp1, mkey->key, 16);

	retv = sub_158(ctx, kbuf, 0x10, tmp1, code);
	if(retv)
		goto _exit;

	for(i=0; i<0x10; i++){
		tmp1[i] ^= loc_1CD4[i];
	}

	if(vkey){
		for(i=0; i<0x10; i++){
			tmp1[i] ^= vkey[i];
		}
		memcpy(kbuf, tmp1, 16);

		retv = do_kirk4(ctx, kbuf, kbuf, 0x10, code);
		if(retv)
			goto _exit;

		memcpy(tmp1, kbuf, 16);
	}

	memcpy(buf, tmp1, 16);

	memset(mkey->key, 0, 16);
	memset(mkey->pad, 0, 16);

	mkey->pad_size = 0;
	mkey->type = 0;
	retv = 0;

_exit:
	return retv;
}

// get key from bbmac
int bbmac_getkey(NkPspCtx *ctx, MAC_KEY *mkey, const u8 *bbmac, u8 *vkey)
{
	int i, retv, type, code;
	u8 kbuf[16], tmp[16], tmp1[16];

	if (ctx == NULL || mkey == NULL || bbmac == NULL || vkey == NULL)
		return NK_PSP_ERR_INTERNAL;
	type = mkey->type;
	retv = sceDrmBBMacFinal(ctx, mkey, tmp, NULL);
	if(retv)
		return retv;

	// decrypt bbmac (type 3: the MAC was encrypted again)
	memcpy(kbuf, bbmac, 0x10);
	if(type==3){
		retv = do_kirk7(ctx, kbuf, kbuf, 0x10, AMCTRL_BBMAC_CODE);
		if(retv)
			return retv;
	}

	memcpy(tmp1, kbuf, 16);

	code = AMCTRL_MAC_CODE;
	retv = do_kirk7(ctx, kbuf, tmp1, 0x10, code);
	if(retv)
		return retv;

	for(i=0; i<0x10; i++){
		vkey[i] = tmp[i] ^ kbuf[i];
	}

	return 0;
}

/*************************************************************/

static int sub_1F8(NkPspCtx *ctx, u8 *out, const u8 *in, int size,
                   const u8 *key, int key_type)
{
	int i, retv;

	retv = do_kirk7(ctx, out, in, size, key_type);
	if(retv)
		return retv;

	for(i=0; i<16; i++){
		out[i] ^= key[i];
	}

	return 0;
}


static int sub_428(NkPspCtx *ctx, u8 *dbuf, int size, CIPHER_KEY *ckey)
{
	int i, retv, blocks;
	u8 tmp1[16], tmp2[16];
	u8 ctr[AMCTRL_CHUNK], kbuf[AMCTRL_CHUNK];
	const u8 *loc_1CE4 = NULL, *loc_1CF4 = NULL;

	if (size <= 0 || size > AMCTRL_CHUNK) return NK_PSP_ERR_INTERNAL;
	retv = amctrl_const(ctx, "amctrl.loc_1CE4", &loc_1CE4);
	if (retv == 0) retv = amctrl_const(ctx, "amctrl.loc_1CF4", &loc_1CF4);
	if (retv)
		return retv;

	for(i=0; i<16; i++){
		tmp2[i] = ckey->key[i] ^ loc_1CF4[i];
	}

	retv = do_kirk7(ctx, tmp2, tmp2, 16, AMCTRL_CIPHER_CODE);
	if(retv)
		return retv;

	for(i=0; i<16; i++){
		tmp2[i] ^= loc_1CE4[i];
	}

	if(ckey->seed==1){
		memset(tmp1, 0, 0x10);
	}else{
		memcpy(tmp1, tmp2, 0x10);
		amctrl_put32le(tmp1+0x0c, ckey->seed-1);
	}

	blocks = (size + 15) & ~15;
	for(i=0; i<blocks; i+=16){
		memcpy(ctr+i, tmp2, 12);
		amctrl_put32le(ctr+i+12, ckey->seed);
		ckey->seed += 1;
	}

	retv = sub_1F8(ctx, kbuf, ctr, blocks, tmp1, AMCTRL_BBMAC_CODE);
	if(retv)
		return retv;

	for(i=0; i<size; i++){
		dbuf[i] ^= kbuf[i];
	}

	return 0;
}

// type: 1 use fixed key
//       2 use fuse id (refused: CONTEXT_REQUIRED)
// mode: 1 for encrypt (not part of the boundary)
//       2 for decrypt
int sceDrmBBCipherInit(NkPspCtx *ctx, CIPHER_KEY *ckey, int type, int mode,
                       const u8 *header_key, const u8 *version_key, u32 seed)
{
	int i;

	if (ctx == NULL || ckey == NULL) return NK_PSP_ERR_INTERNAL;
	memset(ckey, 0, sizeof(*ckey));
	if (type == 2)
		return refuse_fuse_id(ctx, "this DRM BB cipher (type 2)");
	if (type != 1) {
		return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
		                   "DRM BB cipher type %d is not part of the built-in "
		                   "decryption boundary", type);
	}
	if (mode != 2) {
		return nk_psp_fail(ctx, NK_PSP_ERR_UNSUPPORTED, NULL,
		                   "DRM BB cipher mode %d is not part of the built-in "
		                   "decryption boundary (decrypt only)", mode);
	}
	if (header_key == NULL) return NK_PSP_ERR_INTERNAL;

	ckey->type = (u32)type;
	ckey->seed = seed+1;
	for(i=0; i<16; i++){
		ckey->key[i] = header_key[i];
	}
	if(version_key){
		for(i=0; i<16; i++){
			ckey->key[i] ^= version_key[i];
		}
	}

	return 0;
}

int sceDrmBBCipherUpdate(NkPspCtx *ctx, CIPHER_KEY *ckey, u8 *data, int size)
{
	int p, retv, dsize;

	if (ctx == NULL || ckey == NULL || size < 0 || (data == NULL && size != 0))
		return NK_PSP_ERR_INTERNAL;
	if (ckey->type != 1) {
		return nk_psp_fail(ctx, NK_PSP_ERR_INTERNAL, NULL,
		                   "DRM BB cipher state is not initialised");
	}

	retv = 0;
	p = 0;

	while(size>0){
		dsize = (size>=AMCTRL_CHUNK)? AMCTRL_CHUNK : size;
		retv = sub_428(ctx, data+p, dsize, ckey);
		if(retv)
			break;
		size -= dsize;
		p += dsize;
	}

	return retv;
}

int sceDrmBBCipherFinal(CIPHER_KEY *ckey)
{
	if (ckey == NULL) return NK_PSP_ERR_INTERNAL;
	memset(ckey->key, 0, 16);
	ckey->type = 0;
	ckey->seed = 0;

	return 0;
}
