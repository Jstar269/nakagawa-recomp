// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
//
/*
 * in callable form, used to verify the VFPU against reference trace data and as the
 * interpreter fallback (ARCHITECTURE.md section 3). Reuses the runtime prefix/transcendental
 * kernels in recomp.c.
 *
 * sr_vfpu_interp executes one VFPU compute or prefix instruction against the CpuState and
 * returns SR_VFPU_COMPUTE (a value-producing op, compare v[]/f[] against the oracle),
 * SR_VFPU_STATE (a prefix/control or store op), or SR_VFPU_OTHER (not handled here).
 */

#include "recomp.h"

#include <math.h>
#include <string.h>

#define SR_VFPU_OTHER   0
#define SR_VFPU_COMPUTE 1
#define SR_VFPU_STATE   2

/* Physical v[] indices for a VFPU vector register; the C twin of
 * tools/codegen.py vreg_indices().
 *
 * Origin: written from PPSSPP's voffset-integrated addressing
 * (MIPSVFPUUtils.cpp). That lineage is real and is retained.
 *
 * Authority: measured on a PSP-3001 (6.61-ARK, 2/2 reproducible runs) under
 * HQ-1. All 128 scalar encodings and 14 selected wide encodings matched,
 * including the two discriminating cases -- triple width takes its row from
 * bit 6 alone, and transpose wraps as (row + lane) & 3 rather than saturating.
 * See fixtures/vfpu_addressing/hardware_vfpu_addr_001.json and issue #296.
 *
 * Boundary: 14 of 512 wide encodings were observed on silicon. The remainder
 * are covered only by derived cross-implementation tests
 * (src/rt/vfpu_addr_selftest.c driven by tools/test_vfpu_addressing.py), which
 * assert that this decoder and the Python one agree over the whole finite
 * domain. That is a consistency property, not a hardware measurement.
 *
 * mreg_idx() below was NOT probed and still rests on PPSSPP alone. */
static int vreg_idx(int reg, int size, uint8_t *out) {
    int mtx = (reg >> 2) & 7, col = reg & 3, transpose = (reg >> 5) & 1, row, length;
    if (size == 1) { transpose = 0; row = (reg >> 5) & 3; length = 1; }
    else if (size == 2) { row = (reg >> 5) & 2; length = 2; }
    else if (size == 3) { row = (reg >> 6) & 1; length = 3; }
    else { row = (reg >> 5) & 2; length = 4; }
    for (int i = 0; i < length; i++)
        out[i] = transpose ? (uint8_t)(mtx * 16 + ((row + i) & 3) * 4 + col)
                           : (uint8_t)(mtx * 16 + col * 4 + ((row + i) & 3));
    return length;
}

static int mreg_idx(int reg, int side, int j, int i) {
    int mtx = (reg >> 2) & 7, col = reg & 3, transpose = (reg >> 5) & 1, row;
    if (side == 1) { transpose = 0; row = (reg >> 5) & 3; }
    else if (side == 3) row = (reg >> 6) & 1;
    else row = (reg >> 5) & 2;
    return transpose ? mtx * 16 + ((row + i) & 3) * 4 + ((col + j) & 3)
                     : mtx * 16 + ((col + j) & 3) * 4 + ((row + i) & 3);
}

static int vsize(uint32_t w) { return (((w >> 7) & 1) | ((w >> 14) & 2)) + 1; }

static void eat_prefix(CpuState *s) {
    s->vfpuCtrl[0] = 0xe4u;
    s->vfpuCtrl[1] = 0xe4u;
    s->vfpuCtrl[2] = 0u;
}

/* vcst constant table, computed with the same float expressions as PPSSPP (MIPS.cpp). */
static float sr_cst[32];
static int sr_cst_init = 0;
static void sr_cst_load(void) {
    if (sr_cst_init) return;
    /* Literal PSP/PPSSPP float32 results.  Host libm and extended precision must not
     * change vcst bit patterns across compilers. */
    static const uint32_t bits[32] = {
        0x00000000,0x7f7fffff,0x3fb504f3,0x3f3504f3,0x3f906eba,0x3f22f983,0x3ea2f983,0x3f490fdb,
        0x3fc90fdb,0x40490fdb,0x402df854,0x3fb8aa3b,0x3ede5bd9,0x3f317218,0x40135d8e,0x40c90fdb,
        0x3f060a92,0x3e9a209b,0x40549a78,0x3f5db3d7,0,0,0,0,0,0,0,0,0,0,0,0,
    };
    memcpy(sr_cst,bits,sizeof(bits));
    sr_cst_init = 1;
}

int sr_vfpu_interp(CpuState *s, uint32_t w) {
    uint32_t op = w >> 26;

    /* VFPU memory operations, including the left/right quad merge instructions used
     * by ulv.q/usv.q.  Address low bits 0/1 belong to the encoding; effective offsets
     * are therefore sign-extended after masking with 0xFFFC. */
    if (op == 0x32 || op == 0x3a) {  /* lv.s / sv.s */
        int vt=((w>>16)&0x1F)|((w&3)<<5), base=(w>>21)&0x1F;
        uint32_t addr=s->r[base]+(uint32_t)(int32_t)(int16_t)(w&0xFFFCu);
        uint8_t idx[1];vreg_idx(vt,1,idx);
        if(op==0x32){s->vi[idx[0]]=MEM_R32(addr);return SR_VFPU_COMPUTE;}
        MEM_W32(addr,s->vi[idx[0]]);return SR_VFPU_STATE;
    }
    if (op == 0x35 || op == 0x36 || op == 0x3d || op == 0x3e) {
        int vt=((w>>16)&0x1F)|((w&1)<<5),base=(w>>21)&0x1F;
        uint32_t addr=s->r[base]+(uint32_t)(int32_t)(int16_t)(w&0xFFFCu);
        uint8_t idx[4];vreg_idx(vt,4,idx);
        int store=(op==0x3d || op==0x3e);
        /* Quad memory ops are all-or-nothing (issue #184): a span that is not
         * entirely guest-readable (loads) or guest-writable (stores) rejects the
         * whole op BEFORE any destination lane or guest word is committed, so a
         * straddling/wrapped address can never partially mutate guest-visible
         * state. The check reuses the overflow-safe sr_guest_span_* helpers that
         * the scalar accessors already rely on, so the accepted set is identical
         * to per-lane MEM_R32/MEM_W32; only the commit ordering changes. The
         * rejected op returns SR_VFPU_OTHER without consuming S/T/D prefixes. */
        int span_ok;
        uint32_t span_start=addr, span_len=16u;
        if (op == 0x36 || op == 0x3e) {
            /* lv.q / sv.q: the full 16-byte span, from addr upward. */
            span_ok = store ? sr_guest_span_writable(addr, 16u)
                            : sr_guest_span_readable(addr, 16u);
        } else {
            /* lvl/lvr/svl/svr: addr's word offset selects a sub-span of the
             * 16-byte window. Left forms touch [addr-offset*4, addr+4); right
             * forms touch [addr, addr+(4-offset)*4). Checked arithmetic is not
             * required beyond the helpers themselves: sr_inrange_n() validates
             * via phys subtraction, so a wrapped start near UINT32_MAX is
             * rejected, never aliased. */
            int offset=(int)((addr>>2)&3);
            if ((w&2)==0) { span_len=(uint32_t)(offset+1)*4u; span_start=addr-span_len+4u; }
            else          { span_len=(uint32_t)(4-offset)*4u; span_start=addr; }
            span_ok = store ? sr_guest_span_writable(span_start, span_len)
                            : sr_guest_span_readable(span_start, span_len);
        }
        if (!span_ok) {
            sr_oor(span_start, 0, store);
            return SR_VFPU_OTHER;
        }
        if(op==0x36){for(int i=0;i<4;i++)s->vi[idx[i]]=MEM_R32(addr+(uint32_t)i*4);return SR_VFPU_COMPUTE;}
        if(op==0x3e){for(int i=0;i<4;i++)MEM_W32(addr+(uint32_t)i*4,s->vi[idx[i]]);return SR_VFPU_STATE;}
        int offset=(int)((addr>>2)&3);
        if(op==0x35){
            if((w&2)==0){for(int i=0;i<=offset;i++)s->vi[idx[3-i]]=MEM_R32(addr-(uint32_t)i*4);}
            else{for(int i=0;i<=3-offset;i++)s->vi[idx[i]]=MEM_R32(addr+(uint32_t)i*4);}
            return SR_VFPU_COMPUTE;
        }
        if((w&2)==0){for(int i=0;i<=offset;i++)MEM_W32(addr-(uint32_t)i*4,s->vi[idx[3-i]]);}
        else{for(int i=0;i<=3-offset;i++)MEM_W32(addr+(uint32_t)i*4,s->vi[idx[i]]);}
        return SR_VFPU_STATE;
    }

    if (op == 0x37) {  /* (w>>24)&3: 0/1/2 = vpfxs/vpfxt/vpfxd, 3 = viim/vfim */
        int rn = (w >> 24) & 3;
        if (rn == 3) {  /* viim: signed 16-bit int immediate; vfim: float16 immediate */
            int vt = (w >> 16) & 0x7F;
            uint32_t imm = w & 0xFFFF;
            float d[1];
            if ((w >> 23) & 1) {  /* vfim: IEEE half -> float32 */
                uint32_t sgn = (imm >> 15) & 1, e = (imm >> 10) & 0x1F, m = imm & 0x3FF;
                uint32_t bits;
                if (e == 0) {
                    if (m == 0) bits = sgn << 31;
                    else {
                        uint32_t e2 = 127 - 15 + 1;
                        while (!(m & 0x400)) { m <<= 1; e2--; }
                        bits = (sgn << 31) | (e2 << 23) | ((m & 0x3FF) << 13);
                    }
                } else if (e == 31) {
                    bits = (sgn << 31) | (0xFFu << 23) | (m << 13);
                } else {
                    bits = (sgn << 31) | ((e - 15 + 127) << 23) | (m << 13);
                }
                memcpy(d, &bits, 4);
            } else {
                d[0] = (float)(int16_t)imm;
            }
            uint8_t i0[1];
            vreg_idx(vt, 1, i0);
            sr_vwrite(s, i0, d, 1, s->vfpuCtrl[2]);
            eat_prefix(s);
            return SR_VFPU_COMPUTE;
        }
        uint32_t data = w & 0xFFFFF;
        if (rn == 2) data &= 0xFFF;
        s->vfpuCtrl[rn] = data;
        return SR_VFPU_STATE;
    }

    int n = vsize(w);
    int vd = w & 0x7F, vs = (w >> 8) & 0x7F, vt = (w >> 16) & 0x7F;
    uint8_t di[4], si[4], ti[4];

    if (op == 0x1b) {  /* VFPU3: vcmp (CC) / vmin / vmax */
        int s3 = (w >> 23) & 7;
        if (s3 == 0) {  /* vcmp: set the CC register, no v[] output */
            int cond = w & 0xF;
            float a[4], b[4];
            vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
            sr_vread(a, s, si, n, s->vfpuCtrl[0]);
            sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
            int cc = 0, orv = 0, andv = 1, affected = (1 << 4) | (1 << 5);
            for (int i = 0; i < n; i++) {
                float x = a[i], y = b[i];
                int c;
                switch (cond) {
                    case 0: c = 0; break;            case 1: c = x == y; break;
                    case 2: c = x < y; break;        case 3: c = x <= y; break;
                    case 4: c = 1; break;            case 5: c = x != y; break;
                    case 6: c = x >= y; break;       case 7: c = x > y; break;
                    case 8: c = x == 0.0f; break;    case 9: c = isnan(x); break;
                    case 10: c = isinf(x); break;    case 11: c = isnan(x) || isinf(x); break;
                    case 12: c = x != 0.0f; break;   case 13: c = !isnan(x); break;
                    case 14: c = !isinf(x); break;   default: c = !(isnan(x) || isinf(x)); break;
                }
                cc |= (c << i); orv |= c; andv &= c; affected |= 1 << i;
            }
            s->vfpuCtrl[3] = (s->vfpuCtrl[3] & ~affected) | ((cc | (orv << 4) | (andv << 5)) & affected);
            eat_prefix(s); return SR_VFPU_STATE;
        }
        if (s3 == 2 || s3 == 3) {  /* vmin / vmax (PPSSPP Int_Vminmax, with NaN/inf int compare) */
            float a[4], b[4], d[4];
            vreg_idx(vd, n, di); vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
            sr_vread(a, s, si, n, s->vfpuCtrl[0]);
            sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
            for (int i = 0; i < n; i++) {
                int an = isnan(a[i]) || isinf(a[i]), bn = isnan(b[i]) || isinf(b[i]);
                if (an || bn) {
                    int32_t ai, bi;
                    memcpy(&ai, &a[i], 4); memcpy(&bi, &b[i], 4);
                    int32_t r;
                    if (s3 == 2) r = (ai < 0 && bi < 0) ? (bi < ai ? ai : bi) : (ai < bi ? ai : bi);
                    else r = (ai < 0 && bi < 0) ? (ai < bi ? ai : bi) : (bi < ai ? ai : bi);
                    memcpy(&d[i], &r, 4);
                } else {
                    d[i] = s3 == 2 ? (a[i] < b[i] ? a[i] : b[i]) : (b[i] < a[i] ? a[i] : b[i]);
                }
            }
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (s3 == 6 || s3 == 7) {  /* vcmovt / vcmovf */
            int tf = s3 & 1, imm3 = (w >> 16) & 7;
            float sv[4], d[4];
            vreg_idx(vs, n, si); vreg_idx(vd, n, di);
            sr_vread(sv, s, si, n, s->vfpuCtrl[0]);
            sr_vread(d, s, di, n, s->vfpuCtrl[1]);
            uint32_t cc = s->vfpuCtrl[3];
            if (imm3 < 6) {
                if ((int)((cc >> imm3) & 1) == !tf)
                    for (int i = 0; i < n; i++) d[i] = sv[i];
            } else if (imm3 == 6) {
                for (int i = 0; i < n; i++)
                    if ((int)((cc >> i) & 1) == !tf) d[i] = sv[i];
            }
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        return SR_VFPU_OTHER;
    }

    if (op == 0x34) {
        /* Opcode 0x34 sub-dispatch by bits [25:21]: 3 = vcst, 0 = VV2Op, 16-23 = vf2i/vi2f
         * conversions (not handled here). */
        int sub21 = (w >> 21) & 0x1F;
        if (sub21 == 3) {  /* vcst: broadcast a constant */
            sr_cst_load();
            vreg_idx(vd, n, di);
            float d[4], c = sr_cst[(w >> 16) & 0x1F];
            for (int i = 0; i < n; i++) d[i] = c;
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (sub21 == 21) {  /* vcmov: conditional move on the VFPU CC register */
            int tf = (w >> 19) & 1, imm3 = (w >> 16) & 7;
            float sv[4], d[4];
            vreg_idx(vs, n, si); vreg_idx(vd, n, di);
            sr_vread(sv, s, si, n, s->vfpuCtrl[0]);
            sr_vread(d, s, di, n, s->vfpuCtrl[1]);  /* vd is read as T */
            uint32_t cc = s->vfpuCtrl[3];
            if (imm3 < 6) {
                if ((int)((cc >> imm3) & 1) == !tf)
                    for (int i = 0; i < n; i++) d[i] = sv[i];
            } else if (imm3 == 6) {
                for (int i = 0; i < n; i++)
                    if ((int)((cc >> i) & 1) == !tf) d[i] = sv[i];
            }
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (sub21 == 2) {  /* VFPU9 group */
            int op9 = (w >> 16) & 0x1F;
            if (op9 == 4) {  /* vocp: d = 1 - s, computed as forced prefixes per PPSSPP:
                              * S gains negate-all, T is forced to constant ONE (user T abs
                              * may swap the constant, negate still applies). NaN stays
                              * positive. */
                float sv[4], tv[4], d[4];
                vreg_idx(vs, n, si);
                sr_vread(sv, s, si, n, s->vfpuCtrl[0] | (0xFu << 16));
                sr_vread(tv, s, si, n, (s->vfpuCtrl[1] & ~0xFFu) | 0x55u | (0xFu << 12));
                for (int i = 0; i < n; i++)
                    d[i] = isnan(sv[i]) ? fabsf(sv[i]) : tv[i] + sv[i];
                vreg_idx(vd, n, di);
                sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
            }
            return SR_VFPU_OTHER;
        }
        if (sub21 == 1) {  /* VFPU7: integer expand/pack conversions */
            int idx7 = (w >> 16) & 0x1F;
            if (idx7 == 27) {  /* vs2i: each 16-bit half of a lane widens to one 32-bit lane.
                                * Output is double-width: .s -> pair, .p -> quad (PPSSPP
                                * Int_Vx2i). Low half lands in the even lane's high bits. */
                int sz = n >= 3 ? 2 : n, oz = sz * 2;
                uint32_t sv[4], d[4] = {0, 0, 0, 0};
                vreg_idx(vs, n, si);
                sr_vread((float *)sv, s, si, sz, s->vfpuCtrl[0]);
                for (int i = 0; i < sz; i++) {
                    d[i * 2]     = (sv[i] & 0xFFFFu) << 16;
                    d[i * 2 + 1] = sv[i] & 0xFFFF0000u;
                }
                vreg_idx(vd, oz, di);
                sr_vwrite(s, di, (float *)d, oz, s->vfpuCtrl[2]);
                eat_prefix(s); return SR_VFPU_COMPUTE;
            }
            if (idx7 >= 28) {  /* vi2uc (28) / vi2c (29) / vi2us (30) / vi2s (31): pack
                                * 32-bit int lanes into bytes/shorts (PPSSPP Int_Vi2x). */
                int c = idx7 - 28, oz;
                int32_t sv[4];
                uint32_t d[2] = {0, 0};
                vreg_idx(vs, 4, si);
                sr_vread((float *)sv, s, si, 4, s->vfpuCtrl[0]);
                if (c == 0) {          /* vi2uc: clamp negatives to 0, keep top 8 magnitude bits */
                    for (int i = 0; i < 4; i++) {
                        int32_t v = sv[i];
                        if (v < 0) v = 0;
                        v >>= 23;
                        d[0] |= ((uint32_t)v & 0xFFu) << (i * 8);
                    }
                    oz = 1;
                } else if (c == 1) {   /* vi2c: raw top byte of each lane */
                    for (int i = 0; i < 4; i++)
                        d[0] |= (((uint32_t)sv[i] >> 24) & 0xFFu) << (i * 8);
                    oz = 1;
                } else {               /* vi2us (c==2) / vi2s (c==3): two lanes per short pair */
                    int elems = (n + 1) / 2;
                    for (int i = 0; i < elems; i++) {
                        if (c == 2) {
                            int32_t lo = sv[i * 2], hi = sv[i * 2 + 1];
                            if (lo < 0) lo = 0;
                            if (hi < 0) hi = 0;
                            lo >>= 15; hi >>= 15;
                            d[i] = ((uint32_t)lo & 0xFFFFu) | (((uint32_t)hi & 0xFFFFu) << 16);
                        } else {
                            uint32_t lo = (uint32_t)sv[i * 2] >> 16, hi = (uint32_t)sv[i * 2 + 1] >> 16;
                            d[i] = (lo & 0xFFFFu) | (hi << 16);
                        }
                    }
                    oz = n >= 3 ? 2 : 1;
                }
                vreg_idx(vd, oz, di);
                sr_vwrite(s, di, (float *)d, oz, s->vfpuCtrl[2]);
                eat_prefix(s); return SR_VFPU_COMPUTE;
            }
            return SR_VFPU_OTHER;  /* vrnds/vrndi/vrndf/vf2h/vh2f: no static emitter either */
        }
        if (sub21 >= 16 && sub21 <= 19) {  /* vf2in / vf2iz / vf2iu / vf2id: float -> s32
                                            * with 2^imm scale, NaN -> INT_MAX, saturating
                                            * (matches codegen.py vf2i emission bit-for-bit). */
            double mult = (double)(1u << ((w >> 16) & 0x1F));
            float sv[4];
            int32_t d[4];
            vreg_idx(vs, n, si); vreg_idx(vd, n, di);
            sr_vread(sv, s, si, n, s->vfpuCtrl[0]);
            for (int i = 0; i < n; i++) {
                if (isnan(sv[i])) { d[i] = 0x7FFFFFFF; continue; }
                double x = (double)sv[i] * mult;
                if (x > 2147483647.0)            d[i] = 0x7FFFFFFF;
                else if (x <= -2147483648.0)     d[i] = (int32_t)0x80000000;
                else if (sub21 == 16)            d[i] = (int32_t)nearbyint(x);
                else if (sub21 == 17)            d[i] = (int32_t)x;
                else if (sub21 == 18)            d[i] = (int32_t)ceil(x);
                else                             d[i] = (int32_t)floor(x);
            }
            /* PSP ignores destination saturation for the integer result. */
            sr_vwrite(s, di, (float *)d, n, s->vfpuCtrl[2] & 0xFFFFFF00u);
            eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (sub21 == 20) {  /* vi2f: s32 -> float scaled by 2^-imm (exact in float) */
            float mult = 1.0f / (float)(1u << ((w >> 16) & 0x1F));
            int32_t sv[4];
            float d[4];
            vreg_idx(vs, n, si); vreg_idx(vd, n, di);
            sr_vread((float *)sv, s, si, n, s->vfpuCtrl[0]);
            for (int i = 0; i < n; i++) d[i] = (float)sv[i] * mult;
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]);
            eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (sub21 != 0)
            return SR_VFPU_OTHER;
        int optype = (w >> 16) & 0x1F;
        vreg_idx(vd, n, di);
        float d[4];
        if (optype == 3) {  /* vidt */
            int offmask = n >= 3 ? 3 : 1, off = vd & offmask;
            for (int i = 0; i < n; i++) d[i] = (i == off) ? 1.0f : 0.0f;
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (optype == 6 || optype == 7) {  /* vzero / vone */
            for (int i = 0; i < n; i++) d[i] = optype == 6 ? 0.0f : 1.0f;
            sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        float v[4];
        vreg_idx(vs, n, si);
        sr_vread(v, s, si, n, s->vfpuCtrl[0]);
        for (int i = 0; i < n; i++) {
            switch (optype) {
                case 0: d[i] = v[i]; break;
                case 1: d[i] = fabsf(v[i]); break;
                case 2: d[i] = -v[i]; break;
                case 4: d[i] = v[i] <= 0.0f ? 0.0f : (v[i] > 1.0f ? 1.0f : v[i]); break;
                case 5: d[i] = v[i] < -1.0f ? -1.0f : (v[i] > 1.0f ? 1.0f : v[i]); break;
                case 16: d[i] = sr_vfpu_rcp(v[i]); break;
                case 17: d[i] = sr_vfpu_rsqrt(v[i]); break;
                case 18: d[i] = sr_vfpu_sin(v[i]); break;
                case 19: d[i] = sr_vfpu_cos(v[i]); break;
                case 20: d[i] = sr_vfpu_exp2(v[i]); break;
                case 21: d[i] = sr_vfpu_log2(v[i]); break;
                case 22: d[i] = sr_vfpu_sqrt(v[i]); break;
                case 23: d[i] = sr_vfpu_asin(v[i]); break;
                case 24: d[i] = -sr_vfpu_rcp(v[i]); break;
                case 25: d[i] = -sr_vfpu_rsqrt(v[i]); break;
                case 26: d[i] = -sr_vfpu_sin(v[i]); break;
                case 27: d[i] = -sr_vfpu_cos(v[i]); break;
                case 28: d[i] = -sr_vfpu_exp2(v[i]); break;
                case 29: d[i] = -sr_vfpu_log2(v[i]); break;
                case 30: d[i] = -sr_vfpu_sqrt(v[i]); break;
                case 31: d[i] = -sr_vfpu_asin(v[i]); break;
                default: return SR_VFPU_OTHER;
            }
        }
        sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }

    int sub = (w >> 23) & 7;

    if ((op == 0x18 && (sub == 0 || sub == 1 || sub == 7)) || (op == 0x19 && sub == 0)) {
        float a[4], b[4], d[4];
        vreg_idx(vd, n, di); vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
        for (int i = 0; i < n; i++)
            d[i] = op == 0x19 ? a[i] * b[i]
                  : sub == 0 ? a[i] + b[i] : sub == 1 ? a[i] - b[i] : a[i] / b[i];
        sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x19 && sub == 1) {  /* vdot */
        float a[4], b[4], dd[1];
        vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
        uint8_t dst[1]; vreg_idx(vd, 1, dst);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
        float acc = 0.0f;
        for (int i = 0; i < n; i++) acc += a[i] * b[i];
        dd[0] = acc;
        sr_vwrite(s, dst, dd, 1, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x19 && sub == 4) {  /* vhdp */
        float a[4], b[4], dd[1];
        vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
        uint8_t dst[1]; vreg_idx(vd, 1, dst);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
        float acc = 0.0f;
        for (int i = 0; i < n - 1; i++) acc += a[i] * b[i];
        acc += 1.0f * b[n - 1];
        dd[0] = isnan(acc) ? fabsf(acc) : acc;
        sr_vwrite(s, dst, dd, 1, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x19 && sub == 5) {  /* vcrs */
        static const int ss[4] = {1, 2, 0, 3}, ts[4] = {2, 0, 1, 3};
        float a[4], b[4], d[4];
        /* vcrs is the triple-vector form only. Reject reserved widths before
         * reading sources; the lane permutation below uses all triple lanes. */
        if (n != 3) return SR_VFPU_OTHER;
        vreg_idx(vd, n, di); vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
        for (int i = 0; i < n; i++) d[i] = a[ss[i]] * b[ts[i]];
        sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x19 && sub == 2) {  /* vscl */
        float a[4], d[4];
        uint8_t sc[1]; vreg_idx(vt, 1, sc);
        vreg_idx(vd, n, di); vreg_idx(vs, n, si);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        float scalar = s->v[sc[0]];
        for (int i = 0; i < n; i++) d[i] = a[i] * scalar;
        sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }

    if (op == 0x3c && sub == 0) {  /* vmmul */
        int side = n;
        float r[16];
        for (int a = 0; a < side; a++)
            for (int b = 0; b < side; b++) {
                float sum = 0.0f;
                for (int c = 0; c < side; c++)
                    sum += s->v[mreg_idx(vs, side, b, c)] * s->v[mreg_idx(vt, side, a, c)];
                r[a * 4 + b] = sum;
            }
        for (int a = 0; a < side; a++)
            for (int b = 0; b < side; b++)
                s->v[mreg_idx(vd, side, a, b)] = r[a * 4 + b];
        eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x3c && (sub == 1 || sub == 2 || sub == 3)) {  /* vtfm */
        int ins = sub, side = ins + 1, tn = n < ins + 1 ? n : ins + 1;
        vreg_idx(vt, side, ti); vreg_idx(vd, side, di);
        float r[4];
        for (int i = 0; i < side; i++) {
            float sum = 0.0f;
            for (int k = 0; k < tn; k++) sum += s->v[mreg_idx(vs, side, i, k)] * s->v[ti[k]];
            if (ins >= n) sum += s->v[mreg_idx(vs, side, i, ins)];
            r[i] = sum;
        }
        for (int i = 0; i < side; i++) s->v[di[i]] = r[i];
        eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x3c && sub == 4) {  /* vmscl: matrix * scalar. Rows 0..side-2 are copied
                                    * raw; prefixes apply only to the final row, matching
                                    * the vmmov convention and codegen.py's emission. */
        int side = n;
        uint8_t sc[1], src[4], dst[4], scv[4];
        float row[4], t[4], d[4];
        vreg_idx(vt, 1, sc);
        float scalar = s->v[sc[0]];
        for (int i = 0; i < side - 1; i++)
            for (int j = 0; j < side; j++)
                s->v[mreg_idx(vd, side, j, i)] = s->v[mreg_idx(vs, side, j, i)] * scalar;
        for (int j = 0; j < side; j++) {
            src[j] = (uint8_t)mreg_idx(vs, side, j, side - 1);
            dst[j] = (uint8_t)mreg_idx(vd, side, j, side - 1);
            scv[j] = sc[0];
        }
        sr_vread(row, s, src, side, s->vfpuCtrl[0]);
        sr_vread(t, s, scv, side, s->vfpuCtrl[1]);
        for (int j = 0; j < side; j++) d[j] = row[j] * t[j];
        sr_vwrite(s, dst, d, side, s->vfpuCtrl[2]);
        eat_prefix(s); return SR_VFPU_COMPUTE;
    }
    if (op == 0x3c && sub == 7) {
        int idx = (w >> 21) & 0x1F;
        if (idx == 28) {  /* VFPUMatrix1: vmmov (0) / vmidt (3) / vmzero (6) / vmone (7) */
            int which = (w >> 16) & 0xF;
            if (which == 0) { /* vmmov: prefixes apply only to the final row, as on Allegrex. */
                int side = n;
                for (int i = 0; i < side - 1; i++)
                    for (int j = 0; j < side; j++)
                        s->v[mreg_idx(vd, side, j, i)] = s->v[mreg_idx(vs, side, j, i)];
                uint8_t src[4], dst[4];
                float row[4];
                for (int j = 0; j < side; j++) {
                    src[j] = (uint8_t)mreg_idx(vs, side, j, side - 1);
                    dst[j] = (uint8_t)mreg_idx(vd, side, j, side - 1);
                }
                sr_vread(row, s, src, side, s->vfpuCtrl[0]);
                sr_vwrite(s, dst, row, side, s->vfpuCtrl[2]);
                eat_prefix(s); return SR_VFPU_COMPUTE;
            }
            if (which != 3 && which != 6 && which != 7) {
                if (which > 7) return SR_VFPU_OTHER;
                /* which 1/2/4/5: vmscl alias decoded by the static emitter (codegen.py
                 * VFPUMatrix1 "which <= 7" path); scalar register number = which. */
                int side = n;
                uint8_t sc[1], src[4], dst[4], scv[4];
                float row[4], t[4], d[4];
                vreg_idx(which & 7, 1, sc);
                float scalar = s->v[sc[0]];
                for (int i = 0; i < side - 1; i++)
                    for (int j = 0; j < side; j++)
                        s->v[mreg_idx(vd, side, j, i)] = s->v[mreg_idx(vs, side, j, i)] * scalar;
                for (int j = 0; j < side; j++) {
                    src[j] = (uint8_t)mreg_idx(vs, side, j, side - 1);
                    dst[j] = (uint8_t)mreg_idx(vd, side, j, side - 1);
                    scv[j] = sc[0];
                }
                sr_vread(row, s, src, side, s->vfpuCtrl[0]);
                sr_vread(t, s, scv, side, s->vfpuCtrl[1]);
                for (int j = 0; j < side; j++) d[j] = row[j] * t[j];
                sr_vwrite(s, dst, d, side, s->vfpuCtrl[2]);
                eat_prefix(s); return SR_VFPU_COMPUTE;
            }
            int side = n;
            for (int j = 0; j < side; j++)
                for (int i = 0; i < side; i++) {
                    float val = which == 3 ? (i == j ? 1.0f : 0.0f) : (which == 6 ? 0.0f : 1.0f);
                    s->v[mreg_idx(vd, side, j, i)] = val;
                }
            eat_prefix(s); return SR_VFPU_COMPUTE;
        }
        if (idx == 29) {  /* vrot (PPSSPP Int_Vrot). Identity S/T prefixes assumed (ACX never
                           * prefixes it); includes the vd/vs same-register overlap quirk where
                           * the cosine is recomputed from the already-written sine lane. */
            int imm = (w >> 16) & 0x1F;
            int sl = (imm >> 2) & 3, cl = imm & 3;
            uint8_t ai[1]; vreg_idx(vs, 1, ai);
            float ang = s->v[ai[0]];
            float sine = sr_vfpu_sin(ang), cosine = sr_vfpu_cos(ang);
            if (imm & 0x10) sine = -sine;
            float d[4] = {0, 0, 0, 0};
            if (sl == cl) { for (int i = 0; i < n; i++) d[i] = sine; }
            else d[sl] = sine;
            d[cl] = cosine;
            if (((vd >> 2) & 7) == ((vs >> 2) & 7)) {
                /* dest overlaps the source matrix: if the angle register is one of the dest
                 * registers, hardware reads it back after the sine write (reg numbers, not
                 * physical indices, per PPSSPP GetVectorRegs comparison) */
                uint8_t dn[4]={0};
                int mtx = (vd >> 2) & 7, col = vd & 3, row;
                if (n == 2) row = (vd >> 5) & 2;
                else if (n == 3) row = (vd >> 6) & 1;
                else row = (vd >> 5) & 2;
                int transpose = (vd >> 5) & 1;
                for (int i = 0; i < n; i++) {
                    int r = (mtx << 2);
                    if (transpose) r += ((row+i)&3) | (col<<5);
                    else r += col | (((row + i) & 3) << 5);
                    dn[i] = (uint8_t)r;
                }
                for (int i = 0; i < n; i++)
                    if (vs == dn[i]) { d[cl] = sr_vfpu_cos(d[i]); break; }
            }
            vreg_idx(vd, n, di);
            uint32_t dmask=(3u<<cl)|(1u<<(8+cl));
            sr_vwrite(s,di,d,n,s->vfpuCtrl[2]&~dmask);eat_prefix(s);return SR_VFPU_COMPUTE;
        }
        return SR_VFPU_OTHER;
    }
    if (op == 0x3c && sub == 5) {  /* vcrsp.t / vqmul.q (PPSSPP Int_CrossQuat, identity-prefix) */
        float a[4], b[4], d[4];
        vreg_idx(vd, n, di); vreg_idx(vs, n, si); vreg_idx(vt, n, ti);
        sr_vread(a, s, si, n, s->vfpuCtrl[0]);
        sr_vread(b, s, ti, n, s->vfpuCtrl[1]);
        if (n == 3) {
            d[0] = a[1] * b[2] - a[2] * b[1];
            d[1] = a[2] * b[0] - a[0] * b[2];
            d[2] = a[0] * b[1] - a[1] * b[0];
        } else if (n == 4) {
            d[0] =  a[0] * b[3] + a[1] * b[2] - a[2] * b[1] + a[3] * b[0];
            d[1] = -a[0] * b[2] + a[1] * b[3] + a[2] * b[0] + a[3] * b[1];
            d[2] =  a[0] * b[1] - a[1] * b[0] + a[2] * b[3] + a[3] * b[2];
            d[3] = -a[0] * b[0] - a[1] * b[1] - a[2] * b[2] + a[3] * b[3];
        } else {
            return SR_VFPU_OTHER;
        }
        sr_vwrite(s, di, d, n, s->vfpuCtrl[2]); eat_prefix(s); return SR_VFPU_COMPUTE;
    }

    return SR_VFPU_OTHER;
}
