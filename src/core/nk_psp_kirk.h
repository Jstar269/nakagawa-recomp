/* SPDX-License-Identifier: GPL-3.0-or-later
 * Ported from libkirk/kirk_engine.h of John-K/pspdecrypt
 * (https://github.com/John-K/pspdecrypt), commit
 * c156627db7634d395c380c0a9589130f603307fc, licensed GPL-3.0-or-later.
 *
 * Draan proudly presents, with huge help from community:
 * coyotebean, Davee, hitchhikr, kgsws, liquidzigong, Mathieulh, Proxima,
 * SilverSpring -- KIRK-ENGINE, an open-source implementation of the KIRK
 * (PSP crypto engine) algorithms.  See nk_psp_kirk.c for the full notice.
 *
 * Modified for this project: header guard, include of the shared crypto
 * contract, run-time KeyStore threading (every command takes an NkPspCtx),
 * removal of the embedded key vault / fuse-ID / private-key operations, and
 * fail-closed refusal of key generation, signing, PRNG and encryption
 * commands.  No key material remains in this header.
 */

#ifndef NK_PSP_KIRK_H
#define NK_PSP_KIRK_H

#include "nk_psp_crypto.h"

/* Kirk return values */
#define KIRK_OPERATION_SUCCESS 0
#define KIRK_NOT_ENABLED 1
#define KIRK_INVALID_MODE 2
#define KIRK_HEADER_HASH_INVALID 3
#define KIRK_DATA_HASH_INVALID 4
#define KIRK_SIG_CHECK_INVALID 5
#define KIRK_UNK_1 6
#define KIRK_UNK_2 7
#define KIRK_UNK_3 8
#define KIRK_UNK_4 9
#define KIRK_UNK_5 0xA
#define KIRK_UNK_6 0xB
#define KIRK_NOT_INITIALIZED 0xC
#define KIRK_INVALID_OPERATION 0xD
#define KIRK_INVALID_SEED_CODE 0xE
#define KIRK_INVALID_SIZE 0xF
#define KIRK_DATA_SIZE_ZERO 0x10

typedef struct
{
	int mode;    //0
	int unk_4;   //4
	int unk_8;   //8
	int keyseed; //C
	int data_size;   //10
} KIRK_AES128CBC_HEADER; //0x14

typedef struct
{
	u8  AES_key[16];            //0
	u8  CMAC_key[16];           //10
	u8  CMAC_header_hash[16];   //20
	u8  CMAC_data_hash[16];     //30
	u8  unused[32];             //40
	u32 mode;                   //60
	u8  ecdsa_hash;             //64
	u8  unk3[11];               //65
	u32 data_size;              //70
	u32 data_offset;            //74
	u8  unk4[8];                //78
	u8  unk5[16];               //80
} KIRK_CMD1_HEADER; //0x90

typedef struct
{
	u8  AES_key[16];            //0
	u8  header_sig_r[20];       //10
	u8  header_sig_s[20];       //24
	u8  data_sig_r[20];         //38
	u8  data_sig_s[20];         //4C
	u32 mode;                   //60
	u8  ecdsa_hash;             //64
	u8  unk3[11];               //65
	u32 data_size;              //70
	u32 data_offset;            //74
	u8  unk4[8];                //78
	u8  unk5[16];               //80
} KIRK_CMD1_ECDSA_HEADER; //0x90

typedef struct
{
	u8 r[0x14];
	u8 s[0x14];
} ECDSA_SIG; //0x28
typedef struct
{
	u8 x[0x14];
	u8 y[0x14];
} ECDSA_POINT; //0x28

typedef struct
{
	u32 data_size;             //0
} KIRK_SHA1_HEADER;            //4

typedef struct
{
	u8 private_key[0x14];
	ECDSA_POINT public_key;
} KIRK_CMD12_BUFFER;

typedef struct
{
	u8 multiplier[0x14];
	ECDSA_POINT public_key;
} KIRK_CMD13_BUFFER;

typedef struct
{
	u8 enc_private[0x20];               //0
	u8 message_hash[0x14];              //20
} KIRK_CMD16_BUFFER;//0x34

typedef struct
{
	ECDSA_POINT public_key;             //0
	u8 message_hash[0x14];              //28
	ECDSA_SIG signature;                //3C
} KIRK_CMD17_BUFFER;//0x64

/* mode passed to sceUtilsBufferCopyWithRange */
#define KIRK_CMD_DECRYPT_PRIVATE 1
#define KIRK_CMD_2 2
#define KIRK_CMD_3 3
#define KIRK_CMD_ENCRYPT_IV_0 4
#define KIRK_CMD_ENCRYPT_IV_FUSE 5
#define KIRK_CMD_ENCRYPT_IV_USER 6
#define KIRK_CMD_DECRYPT_IV_0 7
#define KIRK_CMD_DECRYPT_IV_FUSE 8
#define KIRK_CMD_DECRYPT_IV_USER 9
#define KIRK_CMD_PRIV_SIGN_CHECK 10
#define KIRK_CMD_SHA1_HASH 11
#define KIRK_CMD_ECDSA_GEN_KEYS 12
#define KIRK_CMD_ECDSA_MULTIPLY_POINT 13
#define KIRK_CMD_PRNG 14
#define KIRK_CMD_15 15
#define KIRK_CMD_ECDSA_SIGN 16
#define KIRK_CMD_ECDSA_VERIFY 17

/* "mode" in header */
#define KIRK_MODE_CMD1 1
#define KIRK_MODE_CMD2 2
#define KIRK_MODE_CMD3 3
#define KIRK_MODE_ENCRYPT_CBC 4
#define KIRK_MODE_DECRYPT_CBC 5

/*
 * Decryption-boundary KIRK commands.  Each takes the operation context so
 * key material is fetched from the KeyStore and a missing entry fails
 * closed with its exact name.  Commands that would need embedded private
 * material (0x0C keygen, 0x0E PRNG, 0x10 sign) or that package content
 * (0x00 encrypt) refuse with KIRK_INVALID_OPERATION.
 */
int kirk_CMD1(NkPspCtx *ctx, u8 *outbuff, u8 *inbuff, int size);
int kirk_CMD4(NkPspCtx *ctx, u8 *outbuff, u8 *inbuff, int size);
int kirk_CMD7(NkPspCtx *ctx, u8 *outbuff, u8 *inbuff, int size);
int kirk_CMD10(NkPspCtx *ctx, u8 *inbuff, int insize);
int kirk_CMD11(NkPspCtx *ctx, u8 *outbuff, u8 *inbuff, int size);
int kirk_CMD12(NkPspCtx *ctx, u8 *outbuff, int outsize);
int kirk_CMD13(NkPspCtx *ctx, u8 *outbuff, int outsize, u8 *inbuff, int insize);
int kirk_CMD14(NkPspCtx *ctx, u8 *outbuff, int outsize);
int kirk_CMD16(NkPspCtx *ctx, u8 *outbuff, int outsize, u8 *inbuff, int insize);
int kirk_CMD17(NkPspCtx *ctx, u8 *inbuff, int insize);

/* Load the KIRK CMD1 wrapping key from the KeyStore. */
int kirk_init(NkPspCtx *ctx);

/* overhead-free helpers (raw AES-CBC passes under a keyvault slot) */
int kirk4(NkPspCtx *ctx, u8 *outbuff, const u8 *inbuff, size_t size, int keyId);
int kirk7(NkPspCtx *ctx, u8 *outbuff, const u8 *inbuff, size_t size, int keyId);

/* sce-like dispatch (ctx-threaded port of sceUtilsBufferCopyWithRange) */
int sceUtilsBufferCopyWithRange(NkPspCtx *ctx, u8 *outbuff, int outsize,
                                u8 *inbuff, int insize, int cmd);

#endif /* NK_PSP_KIRK_H */
