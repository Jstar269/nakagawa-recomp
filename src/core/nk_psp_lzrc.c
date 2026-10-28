/* SPDX-License-Identifier: GPL-2.0-or-later
 *
 * tlzrc.c: LZRC decodeer
 *   based on benhur's code, rewrite by tpu
 *
 * Copyright (c) 2012- PPSSPP Project.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.0 or later versions.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License 2.0 for more details.
 *
 * A copy of the GPL 2.0 should have been included with the program.
 * If not, see http://www.gnu.org/licenses/
 *
 * Official git repository and contact information can be found at
 * https://github.com/hrydgard/ppsspp and http://www.ppsspp.org/.
 *
 * Ported from Core/FileSystems/tlzrc.cpp of PPSSPP at revision
 * b4386554795949491a26e51b7d74082bb9b2470f.  Modified for this project
 * (see src/core/PROVENANCE.md): C99 with const input and size_t lengths
 * (each bounded by NK_PSP_LZRC_MAX_LEN); the logging include and the
 * console message are gone; a match distance is decoded in 64 bits, so a
 * crafted stream cannot overflow it into a negative or wrapped value; a match
 * copy stops at the first byte that does not fit, instead of reading on past
 * the output; a literal that does not fit fails before the last output byte
 * is read back; and a literal-context shift past the byte width selects
 * context 0 instead of shifting by 32 or more (undefined in C).  The
 * probability tables keep upstream's enlarged sizes.
 */

#include <string.h>

#include "nk_psp_lzrc.h"

/*************************************************************/

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/*************************************************************/

typedef struct{

	// input stream
	const u8 *input;
	int in_ptr;
	int in_len;

	// output stream
	u8 *output;
	int out_ptr;
	int out_len;

	// Set when input/output bounds are exceeded; decompression should abort.
	int error;

	// range decode
	u32 range;
	u32 code;
	u32 out_code;
	u8 lc;

	u8 bm_literal[8][256];
	// rc_bittree() is called on &bm_dist_bits[len_bits][dist_state] with a limit of up
	// to 44 (dist_state can be 7), and can index up to limit-1 past that base - i.e.
	// column index up to 7+43=50, so the second dimension must be at least 51 (39 was
	// too small and let a crafted stream corrupt adjacent probability tables).
	u8 bm_dist_bits[8][51];
	// rc_number() is called with &bm_dist[dist_bits][0] where dist_bits can reach 43
	// (see bm_dist_bits above), so the first dimension must be at least 44 (18 was too
	// small).
	u8 bm_dist[44][8];
	u8 bm_match[8][8];
	u8 bm_len[8][31];
}LZRC_DECODE;

/*************************************************************/

static u8 rc_getbyte(LZRC_DECODE *rc)
{
	if(rc->in_ptr >= rc->in_len){
		rc->error = 1;
		return 0;
	}

	return rc->input[rc->in_ptr++];
}

static void rc_putbyte(LZRC_DECODE *rc, u8 byte)
{
	if(rc->out_ptr >= rc->out_len){
		rc->error = 1;
		return;
	}

	rc->output[rc->out_ptr++] = byte;
}

static void rc_init(LZRC_DECODE *rc, u8 *out, int out_len, const u8 *in, int in_len)
{
	u32 b0, b1, b2, b3;

	rc->input = in;
	rc->in_len = in_len;
	rc->in_ptr = 0;

	rc->output = out;
	rc->out_len = out_len;
	rc->out_ptr = 0;

	rc->error = 0;

	rc->range = 0xffffffff;
	rc->lc = rc_getbyte(rc);
	b0 = rc_getbyte(rc);
	b1 = rc_getbyte(rc);
	b2 = rc_getbyte(rc);
	b3 = rc_getbyte(rc);
	rc->code = (b0<<24) | (b1<<16) | (b2<<8) | (b3<<0);
	rc->out_code = 0xffffffff;

	memset(rc->bm_literal,   0x80, sizeof(rc->bm_literal));
	memset(rc->bm_dist_bits, 0x80, sizeof(rc->bm_dist_bits));
	memset(rc->bm_dist,      0x80, sizeof(rc->bm_dist));
	memset(rc->bm_match,     0x80, sizeof(rc->bm_match));
	memset(rc->bm_len,       0x80, sizeof(rc->bm_len));

}



/*************************************************************/

/* range decode */

static void normalize(LZRC_DECODE *rc)
{
	if(rc->range<0x01000000){
		rc->range <<= 8;
		rc->code = (rc->code<<8)+rc_getbyte(rc);
	}
}

/* Decode a bit */
static int rc_bit(LZRC_DECODE *rc, u8 *prob)
{
	u32 bound;

	normalize(rc);

	bound = (rc->range>>8)*(*prob);
	*prob -= *prob>>3;

	if(rc->code < bound){
		rc->range = bound;
		*prob += 31;
		return 1;
	}else{
		rc->code -= bound;
		rc->range -= bound;
		return 0;
	}
}

/* Decode a bittree starting from MSB */
static int rc_bittree(LZRC_DECODE *rc, u8 *probs, int limit)
{
	int number = 1;

	do{
		number = (number<<1)+rc_bit(rc, probs+number);
	}while(number<limit);

	return number;
}


/*
 * decode a number
 *
 * a number are divide into three part:
 *   MSB 2bits
 *   direct bits (don't use probability modle)
 *   LSB 3bits
 */
static unsigned long long rc_number(LZRC_DECODE *rc, u8 *prob, int n)
{
	int i;
	unsigned long long number = 1;

	if(n>3){
		number = (number<<1)+(unsigned)rc_bit(rc, prob+3);
		if(n>4){
			number = (number<<1)+(unsigned)rc_bit(rc, prob+3);
			if(n>5){
				// direct bits
				normalize(rc);

				for(i=0; i<n-5; i++){
					rc->range >>= 1;
					number <<= 1;
					if (rc->code < rc->range){
						number += 1;
					}else{
						rc->code -= rc->range;
					}
				}
			}
		}
	}

	if(n>0){
		number = (number<<1)+(unsigned)rc_bit(rc, prob);
		if(n>1){
			number = (number<<1)+(unsigned)rc_bit(rc, prob+1);
			if(n>2){
				number = (number<<1)+(unsigned)rc_bit(rc, prob+2);
			}
		}
	}

	return number;
}


int nk_psp_lzrc_decompress(uint8_t *out, size_t out_len,
                           const uint8_t *in, size_t in_len)
{
	LZRC_DECODE rc;
	int match_step, rc_state, len_state, dist_state;
	int i, bit, byte;
	u32 last_byte;
	int match_len, len_bits;
	int dist_bits, limit;
	unsigned long long match_dist;
	const u8 *match_src;

	if (out == NULL || in == NULL || out_len > NK_PSP_LZRC_MAX_LEN ||
	    in_len > NK_PSP_LZRC_MAX_LEN)
		return -1;

	rc_init(&rc, out, (int)out_len, in, (int)in_len);

	if(rc.error)
		return -1;

	if(rc.lc&0x80){
		/* plain text */
		u32 copySize = rc.code;
		if (copySize > (u32)rc.out_len) {
			copySize = (u32)rc.out_len;
		}
		if (copySize > (u32)(rc.in_len - 5)) {
			copySize = (u32)(rc.in_len - 5);
		}
		memcpy(rc.output, rc.input+5, copySize);
		return (int)copySize;
	}

	rc_state = 0;
	last_byte = 0;

	while (1) {
		if(rc.error)
			return -1;

		match_step = 0;

		bit = rc_bit(&rc, &rc.bm_match[rc_state][match_step]);
		if (bit==0) {
			/* 0 -> raw char */
			if(rc_state>0)
				rc_state -= 1;

			byte = rc_bittree(&rc, &rc.bm_literal[(rc.lc < 8 ? (last_byte>>rc.lc)&0x07 : 0)][0], 0x100);
			byte -= 0x100;

			rc_putbyte(&rc, (u8)byte);
			if(rc.error)
				return -1;
		} else {
			/* 1 -> a match */

			/* find bits of match length */
			len_bits = 0;
			for(i=0; i<7; i++){
				match_step += 1;
				bit = rc_bit(&rc, &rc.bm_match[rc_state][match_step]);
				if(bit==0)
					break;
				len_bits += 1;
			}

			/* find match length */
			if(len_bits==0){
				match_len = 1;
			}else{
				len_state = ((len_bits-1)<<2)+(int)(((u32)rc.out_ptr<<(len_bits-1))&0x03);
				match_len = (int)rc_number(&rc, &rc.bm_len[rc_state][len_state], len_bits);
				if (match_len == 0xFF){
					//end of stream
					return rc.out_ptr;
				}
			}

			/* find number of bits of match distance */
			dist_state = 0;
			limit = 8;
			if(match_len>2){
				dist_state += 7;
				limit = 44;
			}
			dist_bits = rc_bittree(&rc, &rc.bm_dist_bits[len_bits][dist_state], limit);
			dist_bits -= limit;

			/* find match distance */
			if(dist_bits>0){
				match_dist = rc_number(&rc, &rc.bm_dist[dist_bits][0], dist_bits);
			} else {
				match_dist = 1;
			}

			/* copy match bytes */
			if(match_dist>(unsigned long long)rc.out_ptr){
				return -1;
			}
			match_src = rc.output+rc.out_ptr-(int)match_dist;
			for(i=0; i<match_len+1; i++){
				rc_putbyte(&rc, *match_src++);
				if(rc.error)
					return -1;
			}
			rc_state = 6+((rc.out_ptr+1)&1);
		}
		last_byte = rc.output[rc.out_ptr-1];
	}
}
