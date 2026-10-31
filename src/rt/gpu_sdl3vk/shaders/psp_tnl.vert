#version 450
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Copyright (C) 2026 the Nakagawa Recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Translates src/rt/ge.c's transform and lighting (decode_model_vtx skinning, transform_model_vtx_clip,
// light_vertex, project_cvtx) and ge_gpu.c's put_vert into GLSL.
// Modified by Nakagawa Recomp contributors, 2026-10-31.
// See NOTICE.md for upstream lineage and modification provenance.
// Derived from PPSSPP (https://github.com/hrydgard/ppsspp), GPL-2.0-or-later
// Copyright (c) 2012- PPSSPP Project and contributors

/* PSP GE vertex stage with transform and lighting (#318). The CPU path (psp.vert) receives
 * vertices ge.c already transformed and lit; this stage receives the decoded model-space vertex
 * (ge_shared.h GeTnlVtx) and the PRIM's parameter record (GeTnlParams) and computes the same
 * values: skinning (decode_model_vtx), world, view and projection, fog and texture coordinates
 * (transform_model_vtx_clip), light_vertex, the perspective divide and viewport (project_cvtx)
 * and the backend's vertex capture (put_vert: the 28.4 snap and the colour clamp). ge.c offers
 * a PRIM here only when no vertex can be clipped, dropped or non-finite, so there is no
 * acceptance step.
 *
 * Bit-exact by construction, under the contract ge.c checks (every input 0 or normal, of
 * magnitude 2^-40..2^40):
 *   - Vulkan rounds add, subtract and multiply correctly, and every result here is `precise`
 *     (no contraction into fused multiply-add), evaluated in C's left-to-right order. The CPU
 *     reference is built for baseline x86-64, which has no fused multiply-add either.
 *   - Intermediate values can still be subnormal (a squared length of a small transformed
 *     normal, by cancellation or scale), which the CPU keeps. The DenormPreserve execution mode
 *     (SPV_KHR_float_controls) keeps them here too; the backend runs this stage only on a device
 *     that reports shaderDenormPreserveFloat32.
 *   - Division and square root are not correctly rounded in Vulkan, so div_rn and sqrt_rn
 *     compute them exactly in integer arithmetic, rounding to nearest even, for subnormal
 *     operands and quotients as well.
 *   - Constants that C folds (1.0f/255.0f) are their float bit patterns. */

#extension GL_EXT_spirv_intrinsics : require
spirv_execution_mode(extensions = ["SPV_KHR_float_controls"], capabilities = [4464], 4459, 32);

layout(location = 0) in vec3  in_pos;
layout(location = 1) in vec3  in_nrm;
layout(location = 2) in vec2  in_uv;
layout(location = 3) in uvec4 in_color;    /* R8G8B8A8_UINT */
layout(location = 4) in vec4  in_w0;
layout(location = 5) in vec4  in_w1;
layout(location = 6) in uint  in_param;    /* record index, relative to gl_InstanceIndex */

/* GeTnlParams, as 32-bit words. Offsets (in words) follow the C struct. */
layout(std430, set = 1, binding = 0) readonly buffer TnlParams { uint words[]; } tnl;
#define P_WORDS    227
#define P_WORLD    0
#define P_VIEW     12
#define P_PROJ     24
#define P_BONE     40
#define P_VP       136
#define P_OFF      142
#define P_TEX      144
#define P_FOG      150
#define P_FOGMODE  153
#define P_WEIGHTS  154
#define P_LIGHTING 155
#define P_MATUPD   156
#define P_LITALPHA 157
#define P_AMBALPHA 158
#define P_MATAMB   159
#define P_MATDIF   162
#define P_EMISSIVE 165
#define P_AMB      168
#define P_LIGHT    171
#define L_WORDS    14     /* on, point, l[3], acol[3], dcol[3], att[3] */

layout(location = 0) noperspective out vec2  v_uv;
layout(location = 1) noperspective out float v_rw;
layout(location = 2) noperspective out float v_fog;
layout(location = 3) noperspective out vec4  v_color;

precise gl_Position;

uint s_base;
float pf(int i) { return uintBitsToFloat(tnl.words[s_base + uint(i)]); }
uint  pu(int i) { return tnl.words[s_base + uint(i)]; }

/* The significand of a nonzero float as an integer with its leading one at bit 23, and the
 * biased exponent e for which |a| = m * 2^(e - 150); a subnormal is normalised. */
uint sig24(uint ua, out int e) {
    uint f = (ua >> 23) & 0xffu, m = ua & 0x7fffffu;
    if (f != 0u) { e = int(f); return m | 0x800000u; }
    int sh = 23 - findMSB(m);
    e = 1 - sh;
    return m << uint(sh);
}

/* a / b rounded to nearest even, for a zero, normal or subnormal a and a nonzero finite b
 * whose quotient does not overflow: restoring division of the 24-bit significands, 24 bits
 * plus a round bit, and a sticky bit; a quotient below the normal range is shifted into the
 * subnormal range before it is rounded. */
float div_rn(float a, float b) {
    uint ua = floatBitsToUint(a), ub = floatBitsToUint(b);
    uint sign = (ua ^ ub) & 0x80000000u;
    if ((ua & 0x7fffffffu) == 0u) return uintBitsToFloat(sign);
    int ea, eb;
    uint ma = sig24(ua, ea), mb = sig24(ub, eb);
    int e = ea - eb + 127;
    if (ma < mb) { ma <<= 1; e -= 1; }
    uint q = 0u, r = ma;
    for (int i = 0; i < 25; i++) {
        q <<= 1;
        if (r >= mb) { r -= mb; q |= 1u; }
        r <<= 1;
    }
    uint sticky = r != 0u ? 1u : 0u;
    if (e < 1) {
        int sh = 1 - e;
        if (sh > 25) return uintBitsToFloat(sign);
        if ((q & ((1u << uint(sh)) - 1u)) != 0u) sticky = 1u;
        q >>= uint(sh);
        e = 0;
    }
    uint round_bit = q & 1u;
    q >>= 1;
    if (round_bit != 0u && (sticky != 0u || (q & 1u) != 0u)) q += 1u;
    if (e == 0) return uintBitsToFloat(sign | q);     /* 0x800000 is the smallest normal */
    if (q == 0x1000000u) { q >>= 1; e += 1; }
    return uintBitsToFloat(sign | (uint(e) << 23) | (q & 0x7fffffu));
}

/* sqrt(a) rounded to nearest even, for a zero, positive normal or positive subnormal a:
 * digit-by-digit square root of the significand scaled to 50 bits, giving 24 bits plus a round
 * bit, and a sticky bit. The root of a subnormal is normal. */
float sqrt_rn(float a) {
    uint ua = floatBitsToUint(a);
    if ((ua & 0x7fffffffu) == 0u) return a;
    int ef;
    uint m = sig24(ua, ef);
    int ee = ef - 150;                               /* a = m * 2^ee */
    if ((ee & 1) != 0) { m <<= 1; ee -= 1; } else { m <<= 2; ee -= 2; }   /* m < 2^26 */
    uint rem = 0u, root = 0u;
    for (int k = 0; k < 25; k++) {                   /* the radicand is m * 2^24 */
        int bit = 48 - 2 * k;
        uint pair = bit >= 24 ? (m >> uint(bit - 24)) & 3u : 0u;
        rem = (rem << 2) | pair;
        uint trial = (root << 2) | 1u;
        root <<= 1;
        if (rem >= trial) { rem -= trial; root |= 1u; }
    }
    uint round_bit = root & 1u;
    uint q = root >> 1;
    int er = ee / 2 + 139;
    if (round_bit != 0u && (rem != 0u || (q & 1u) != 0u)) q += 1u;
    if (q == 0x1000000u) { q >>= 1; er += 1; }
    return uintBitsToFloat((uint(er) << 23) | (q & 0x7fffffu));
}

float clampf01(float v) { return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v; }
int clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

/* ge.c mat43_mul: o = m[0..2] x + m[3..5] y + m[6..8] z + m[9..11], left to right. */
vec3 mat43(int at, vec3 p) {
    precise float ox = pf(at + 0) * p.x + pf(at + 3) * p.y + pf(at + 6) * p.z + pf(at + 9);
    precise float oy = pf(at + 1) * p.x + pf(at + 4) * p.y + pf(at + 7) * p.z + pf(at + 10);
    precise float oz = pf(at + 2) * p.x + pf(at + 5) * p.y + pf(at + 8) * p.z + pf(at + 11);
    return vec3(ox, oy, oz);
}

/* ge.c mat33_mul: the upper 3x3 only. */
vec3 mat33(int at, vec3 p) {
    precise float ox = pf(at + 0) * p.x + pf(at + 3) * p.y + pf(at + 6) * p.z;
    precise float oy = pf(at + 1) * p.x + pf(at + 4) * p.y + pf(at + 7) * p.z;
    precise float oz = pf(at + 2) * p.x + pf(at + 5) * p.y + pf(at + 8) * p.z;
    return vec3(ox, oy, oz);
}

/* ge.c mat44_mul on (x, y, z, 1). */
vec4 mat44(int at, vec3 p) {
    precise float ox = pf(at + 0) * p.x + pf(at + 4) * p.y + pf(at + 8)  * p.z + pf(at + 12);
    precise float oy = pf(at + 1) * p.x + pf(at + 5) * p.y + pf(at + 9)  * p.z + pf(at + 13);
    precise float oz = pf(at + 2) * p.x + pf(at + 6) * p.y + pf(at + 10) * p.z + pf(at + 14);
    precise float ow = pf(at + 3) * p.x + pf(at + 7) * p.y + pf(at + 11) * p.z + pf(at + 15);
    return vec4(ox, oy, oz, ow);
}

/* ge_gpu.c snap16: floorf(v * 16 + 0.375) / 16. */
float snap16(float v) {
    precise float s = floor(v * 16.0 + 0.375) * 0.0625;
    return s;
}

void main() {
    s_base = (in_param + uint(gl_InstanceIndex)) * uint(P_WORDS);
    const float k_inv255 = uintBitsToFloat(0x3b808081u);   /* 1.0f / 255.0f */

    /* decode_model_vtx: every weight that is not 0, bone by bone. */
    precise vec3 pos = in_pos;
    precise vec3 nrm = in_nrm;
    int nw = int(pu(P_WEIGHTS));
    if (nw > 0) {
        float w[8] = float[8](in_w0.x, in_w0.y, in_w0.z, in_w0.w, in_w1.x, in_w1.y, in_w1.z, in_w1.w);
        precise float px = 0.0, py = 0.0, pz = 0.0, nx = 0.0, ny = 0.0, nz = 0.0;
        for (int i = 0; i < nw; i++) {
            if (w[i] == 0.0) continue;
            vec3 t = mat43(P_BONE + 12 * i, in_pos);
            px = px + t.x * w[i]; py = py + t.y * w[i]; pz = pz + t.z * w[i];
            vec3 tn = mat33(P_BONE + 12 * i, in_nrm);
            nx = nx + tn.x * w[i]; ny = ny + tn.y * w[i]; nz = nz + tn.z * w[i];
        }
        pos = vec3(px, py, pz);
        nrm = vec3(nx, ny, nz);
    }

    /* transform_model_vtx_clip */
    vec3 wp = mat43(P_WORLD, pos);
    vec3 cp = mat43(P_VIEW, wp);
    vec4 clip = mat44(P_PROJ, cp);

    float fog = 1.0;
    uint fog_mode = pu(P_FOGMODE);
    if (fog_mode == 1u) {
        fog = pf(P_FOG + 2);
    } else if (fog_mode == 2u) {
        precise float f = (cp.z + pf(P_FOG + 0)) * pf(P_FOG + 1);
        fog = clampf01(f);
    }

    precise float u = (in_uv.x * pf(P_TEX + 0) + pf(P_TEX + 2)) * pf(P_TEX + 4);
    precise float tv = (in_uv.y * pf(P_TEX + 1) + pf(P_TEX + 3)) * pf(P_TEX + 5);

    int r = int(in_color.r), g = int(in_color.g), b = int(in_color.b), a = int(in_color.a);
    if (pu(P_LIGHTING) != 0u) {
        /* the world-space normal, normalized */
        vec3 wn = mat33(P_WORLD, nrm);
        precise float nl2 = wn.x * wn.x + wn.y * wn.y + wn.z * wn.z;
        float nl = sqrt_rn(nl2);
        float wnx = wn.x, wny = wn.y, wnz = wn.z;
        if (nl > 0.0) { wnx = div_rn(wnx, nl); wny = div_rn(wny, nl); wnz = div_rn(wnz, nl); }

        /* light_vertex */
        uint mupd = pu(P_MATUPD);
        precise float vrf = float(r) * k_inv255, vgf = float(g) * k_inv255, vbf = float(b) * k_inv255;
        int la = (mupd & 1u) != 0u ? clamp8((a * int(pu(P_AMBALPHA)) + 127) / 255) : int(pu(P_LITALPHA));
        float mar = pf(P_MATAMB + 0), mag = pf(P_MATAMB + 1), mab = pf(P_MATAMB + 2);
        float mdr = pf(P_MATDIF + 0), mdg = pf(P_MATDIF + 1), mdb = pf(P_MATDIF + 2);
        if ((mupd & 1u) != 0u) { mar = vrf; mag = vgf; mab = vbf; }
        if ((mupd & 2u) != 0u) { mdr = vrf; mdg = vgf; mdb = vbf; }
        precise float lr = pf(P_EMISSIVE + 0) + pf(P_AMB + 0) * mar;
        precise float lg = pf(P_EMISSIVE + 1) + pf(P_AMB + 1) * mag;
        precise float lb = pf(P_EMISSIVE + 2) + pf(P_AMB + 2) * mab;
        for (int i = 0; i < 4; i++) {
            int l = P_LIGHT + L_WORDS * i;
            if (pu(l + 0) == 0u) continue;
            precise float lx = pf(l + 2), ly = pf(l + 3), lz = pf(l + 4);
            float att = 1.0;
            if (pu(l + 1) != 0u) {   /* point or spot: from the vertex, attenuated */
                lx = lx - wp.x; ly = ly - wp.y; lz = lz - wp.z;
                precise float d2 = lx * lx + ly * ly + lz * lz;
                float d = sqrt_rn(d2);
                precise float den = pf(l + 11) + pf(l + 12) * d + pf(l + 13) * d2;
                att = den > 0.0 ? clampf01(div_rn(1.0, den)) : 1.0;
                if (d > 0.0) { lx = div_rn(lx, d); ly = div_rn(ly, d); lz = div_rn(lz, d); }
            }
            precise float ndl = wnx * lx + wny * ly + wnz * lz;
            if (ndl < 0.0) ndl = 0.0;
            precise float addr = att * (pf(l + 5) * mar + pf(l + 8) * mdr * ndl);
            precise float addg = att * (pf(l + 6) * mag + pf(l + 9) * mdg * ndl);
            precise float addb = att * (pf(l + 7) * mab + pf(l + 10) * mdb * ndl);
            lr = lr + addr; lg = lg + addg; lb = lb + addb;
        }
        precise float sr = lr * 255.0 + 0.5, sg = lg * 255.0 + 0.5, sb = lb * 255.0 + 0.5;
        r = clamp8(int(sr)); g = clamp8(int(sg)); b = clamp8(int(sb)); a = clamp8(la);
    }

    /* project_cvtx */
    float inv_w = div_rn(1.0, clip.w);
    precise float ndcx = clip.x * inv_w, ndcy = clip.y * inv_w, ndcz = clip.z * inv_w;
    precise float sx = ndcx * pf(P_VP + 0) + pf(P_VP + 3) - pf(P_OFF + 0);
    precise float sy = ndcy * pf(P_VP + 1) + pf(P_VP + 4) - pf(P_OFF + 1);
    precise float szr = ndcz * pf(P_VP + 2) + pf(P_VP + 5);
    float sz = szr < 0.0 ? 0.0 : szr > 65535.0 ? 65535.0 : szr;
    precise float uw = u * inv_w, vw = tv * inv_w;

    /* put_vert, then psp.vert */
    float x = snap16(sx), y = snap16(sy);
    gl_Position = vec4(x * (1.0 / 256.0) - 1.0,
                       y * (1.0 / 136.0) - 1.0,
                       sz * (1.0 / 65535.0),
                       1.0);
    v_uv    = vec2(uw, vw);
    v_rw    = inv_w;
    v_fog   = fog;
    v_color = vec4(div_rn(float(clamp8(r)), 255.0), div_rn(float(clamp8(g)), 255.0),
                   div_rn(float(clamp8(b)), 255.0), div_rn(float(clamp8(a)), 255.0));
}
