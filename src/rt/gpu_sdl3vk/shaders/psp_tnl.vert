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
 * (transform_model_vtx_clip), light_vertex (its specular and powered-diffuse power with ge_powf,
 * the same text as ge.c's), the perspective divide and viewport (project_cvtx)
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
#define P_WORDS    247
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
#define P_MATSPE   171
#define P_SPECCOEF 174
#define P_LIGHT    175
#define L_WORDS    18     /* on, point, l[3], acol[3], dcol[3], att[3], comp, scol[3] */

layout(location = 0) noperspective out vec2  v_uv;
layout(location = 1) noperspective out float v_rw;
layout(location = 2) noperspective out float v_fog;
layout(location = 3) noperspective out vec4  v_color;
layout(location = 4) flat out vec4 v_uvbox;

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

/* ge_powf (ge.c): the specular and powered-diffuse power, in the subset of C and GLSL the two
 * share. The blocks between the BEGIN and END markers are the same text as in ge.c; these
 * definitions give GLSL the helper names that text uses. */
#define GE_POW_FN
uvec2 ge_v2(uint h, uint l) { return uvec2(h, l); }
int ge_i(uint v) { return int(v); }
uint ge_u(int v) { return uint(v); }
uint ge_pow_mulhi(uint a, uint b) { uint hi, lo; umulExtended(a, b, hi, lo); return hi; }
const uint GE_POW_R[92] = uint[92](
    0xB60B60B7u, 0xB40B40B5u, 0xB21642C9u, 0xB02C0B03u, 0xAE4C415Du, 0xAC769185u,
    0xAAAAAAABu, 0xA8E83F58u, 0xA72F053Au, 0xA57EB503u, 0xA3D70A3Eu, 0xA237C32Cu,
    0xA0A0A0A1u, 0x9F1165E8u, 0x9D89D89Eu, 0x9C09C09Du, 0x9A90E7DAu, 0x991F1A52u,
    0x97B425EEu, 0x964FDA6Du, 0x94F20950u, 0x939A85C5u, 0x92492493u, 0x90FDBC0Au,
    0x8FB823EFu, 0x8E78356Eu, 0x8D3DCB09u, 0x8C08C08Du, 0x8AD8F2FCu, 0x89AE408Au,
    0x88888889u, 0x8767AB60u, 0x864B8A7Eu, 0x85340854u, 0x84210843u, 0x83126E98u,
    0x82082083u, 0x81020409u, 0x80000000u, 0x7F01FC08u, 0x7E07E07Fu, 0x7D11967Au,
    0x7C1F07C2u, 0x7B301ECDu, 0x7A44C6B0u, 0x795CEB25u, 0x78787879u, 0x77975B90u,
    0x76B981DBu, 0x75DED953u, 0x75075076u, 0x7432D63Eu, 0x73615A25u, 0x7292CC16u,
    0x71C71C72u, 0x70FE3C08u, 0x70381C0Fu, 0x6F74AE27u, 0x6EB3E454u, 0x6DF5B0F8u,
    0x6D3A06D4u, 0x6C80D902u, 0x6BCA1AF3u, 0x6B15C06Cu, 0x6A63BD82u, 0x69B4069Cu,
    0x6906906Au, 0x685B4FE6u, 0x67B23A55u, 0x670B453Cu, 0x66666667u, 0x65C393E1u,
    0x6522C3F4u, 0x6483ED28u, 0x63E7063Fu, 0x634C0635u, 0x62B2E43Eu, 0x621B97C3u,
    0x61861862u, 0x60F25DEBu, 0x60606061u, 0x5FD017F5u, 0x5F417D06u, 0x5EB48824u,
    0x5E293206u, 0x5D9F7391u, 0x5D1745D2u, 0x5C90A1FEu, 0x5C0B8171u, 0x5B87DDAEu,
    0x5B05B05Cu, 0x5A84F346u
);
const uint GE_POW_TH[92] = uint[92](
    0xBEF50AD0u, 0xC0FF6A2Du, 0xC3041402u, 0xC50327EAu, 0xC6FCC479u, 0xC8F1074Fu,
    0xCAE00D1Cu, 0xCCC9F1A9u, 0xCEAECFE9u, 0xD08EC1F9u, 0xD269E12Eu, 0xD440461Bu,
    0xD6120899u, 0xD7DF3FCFu, 0xD9A80238u, 0xDB6C65A8u, 0xDD2C7F58u, 0xDEE863E4u,
    0xE0A02755u, 0xE253DD2Au, 0xE4039856u, 0xE5AF6B49u, 0xE75767F4u, 0xE8FB9FCDu,
    0xEA9C23D4u, 0xEC390499u, 0xEDD2523Du, 0xEF681C71u, 0xF0FA728Bu, 0xF2896372u,
    0xF414FDB3u, 0xF59D4F7Fu, 0xF72266ACu, 0xF8A450B7u, 0xFA231ACCu, 0xFB9ED1C6u,
    0xFD17822Du, 0xFE8D3844u, 0x00000000u, 0x016FE50Bu, 0x02DCF2CFu, 0x04473474u,
    0x05AEB4DDu, 0x07137EACu, 0x08759C4Fu, 0x09D517EDu, 0x0B31FB7Cu, 0x0C8C50B6u,
    0x0DE42120u, 0x0F397608u, 0x108C588Bu, 0x11DCD196u, 0x132AE9E0u, 0x1476A9F8u,
    0x15C01A39u, 0x170742D3u, 0x184C2BCEu, 0x198EDD06u, 0x1ACF5E2Cu, 0x1C0DB6CCu,
    0x1D49EE4Bu, 0x1E840BE6u, 0x1FBC16B8u, 0x20F215B5u, 0x22260FB5u, 0x23580B63u,
    0x24880F54u, 0x25B621F8u, 0x26E2499Bu, 0x280C8C75u, 0x2934F096u, 0x2A5B7BF7u,
    0x2B803472u, 0x2CA31FC7u, 0x2DC4439Au, 0x2EE3A574u, 0x30014AC5u, 0x311D38E5u,
    0x32377511u, 0x33500471u, 0x3466EC13u, 0x357C30F1u, 0x368FD7EEu, 0x37A1E5D2u,
    0x38B25F5Au, 0x39C14924u, 0x3ACEA7BFu, 0x3BDA7FA6u, 0x3CE4D542u, 0x3DEDACE4u,
    0x3EF50ACFu, 0x3FFAF333u
);
const uint GE_POW_TL[92] = uint[92](
    0x9DE8DE1Du, 0x635E9463u, 0xABCB01ECu, 0x4537E136u, 0xE99B246Eu, 0x960D1402u,
    0xA196263Bu, 0xD48889C9u, 0xEA7B4B22u, 0xEB5D97B6u, 0x92D93835u, 0x192F2E6Du,
    0xF4CFBBEEu, 0x7A3CA98Bu, 0x93A382D6u, 0xB4F8820Au, 0x73E87040u, 0x0A115F73u,
    0xCDAD2B4Du, 0xECC6E3F3u, 0xEB0BDB3Cu, 0x7E1B3CD8u, 0x596E03A6u, 0x06868946u,
    0xC2EEB845u, 0xC147BD63u, 0x1B1636B0u, 0xD3ED2660u, 0x1316EFD4u, 0x372745B8u,
    0xF68D6608u, 0xB6A8E33Cu, 0xDD35C662u, 0x2E904B74u, 0x3AF53F6Fu, 0x2D104E32u,
    0xDC14D474u, 0x87AC478Bu, 0x00000000u, 0x57DB3DB3u, 0x4CCB20B9u, 0x07686F0Du,
    0x4CAA1A67u, 0xCE73B87Fu, 0x74FAAFC5u, 0x19FC7055u, 0x94CA08B0u, 0xF4EF1E8Du,
    0x2E70A041u, 0x8EC5A157u, 0x521025E0u, 0xE786683Eu, 0xF609AE42u, 0xAE7278CAu,
    0x9F816A0Bu, 0x6698C173u, 0x9549FFBCu, 0x5B0439A7u, 0x156D8ED5u, 0xDB63DCE9u,
    0x90C47C05u, 0xC9AFF242u, 0x32A88796u, 0xCC6BB175u, 0x100C9B85u, 0xD52C556Bu,
    0x657A2684u, 0x72D021EDu, 0xF521FCA0u, 0x71D3B410u, 0x8537BD3Fu, 0x24F647BEu,
    0xCB986F1Du, 0x74B81EDEu, 0x30A507A4u, 0x87A969E3u, 0x962E5854u, 0x2877524Bu,
    0x57594776u, 0xD749460Fu, 0xCCE6AF3Fu, 0x1C625F66u, 0x59EFF9E3u, 0xDCC0F3B1u,
    0x19017A80u, 0x1B3E84EBu, 0x50D4C53Fu, 0xBC885692u, 0x29654D24u, 0x674013D7u,
    0x9A397AEDu, 0x9C7010FCu
);
const uint GE_POW_EH[64] = uint[64](
    0x80000000u, 0x8164D1F3u, 0x82CD8698u, 0x843A28C3u, 0x85AAC367u, 0x871F6196u,
    0x88980E80u, 0x8A14D575u, 0x8B95C1E3u, 0x8D1ADF5Bu, 0x8EA4398Bu, 0x9031DC43u,
    0x91C3D373u, 0x935A2B2Fu, 0x94F4EFA8u, 0x96942D37u, 0x9837F051u, 0x99E04593u,
    0x9B8D39B9u, 0x9D3ED9A7u, 0x9EF53260u, 0xA0B0510Fu, 0xA2704303u, 0xA43515AEu,
    0xA5FED6A9u, 0xA7CD93B4u, 0xA9A15AB4u, 0xAB7A39B5u, 0xAD583EEAu, 0xAF3B78ADu,
    0xB123F581u, 0xB311C412u, 0xB504F333u, 0xB6FD91E3u, 0xB8FBAF47u, 0xBAFF5AB2u,
    0xBD08A39Fu, 0xBF1799B6u, 0xC12C4CCAu, 0xC346CCDAu, 0xC5672A11u, 0xC78D74C8u,
    0xC9B9BD86u, 0xCBEC14FEu, 0xCE248C15u, 0xD06333DAu, 0xD2A81D91u, 0xD4F35AABu,
    0xD744FCCAu, 0xD99D15C2u, 0xDBFBB797u, 0xDE60F482u, 0xE0CCDEECu, 0xE33F8972u,
    0xE5B906E7u, 0xE8396A50u, 0xEAC0C6E7u, 0xED4F301Eu, 0xEFE4B99Bu, 0xF281773Cu,
    0xF5257D15u, 0xF7D0DF73u, 0xFA83B2DBu, 0xFD3E0C0Cu
);
const uint GE_POW_EL[64] = uint[64](
    0x00000000u, 0xBC030773u, 0xAC2BA1D7u, 0xACDE4046u, 0xCC487B15u, 0x9E8D1010u,
    0x92DA8527u, 0x496EFD9Au, 0xEA8BD6E7u, 0x7E5BA9E6u, 0x45CD53C0u, 0x1466B1DCu,
    0xAB11C336u, 0x13E6E92Cu, 0xFEF70961u, 0x20185A00u, 0x8DB8A96Fu, 0x20B7FA65u,
    0xD54E5539u, 0x2CFFB751u, 0x91A111AEu, 0xB9714FC2u, 0x0C496819u, 0x09E6809Eu,
    0xB15138EAu, 0xE965356Au, 0xEA7C0EF8u, 0xA93ED337u, 0x42A14AC6u, 0x690A4375u,
    0xD2AC2590u, 0xA9112489u, 0xF9DE6484u, 0x28D17791u, 0x62FB9EE9u, 0x133E45FBu,
    0x580C36BFu, 0x7A731083u, 0x66709456u, 0x24976407u, 0x5506DADDu, 0xABB9B15Du,
    0x6E2F27A3u, 0xF2727C5Du, 0x1F8480E4u, 0xEF2B2595u, 0xF12AE45Au, 0xCFEDFA1Fu,
    0xD69D6AF4u, 0x78AFD7B6u, 0xDAF23755u, 0x5E0E9124u, 0x2A94E111u, 0xBE8A5A51u,
    0x7C8348A8u, 0x3C4BDC68u, 0xDD24392Fu, 0xD9942B84u, 0xDCDAF5CBu, 0x59FFB13Au,
    0x2486CC2Cu, 0x0AD13BB9u, 0x722A033Au, 0xF486C175u
);
/* ---- BEGIN ge_pow helpers (shared text: identical in src/rt/ge.c and shaders/psp_tnl.vert) ---- */
/* x^y for the GE's specular and powered-diffuse terms, in unsigned 32-bit integer arithmetic
 * only, so the C and GLSL copies give the same bits with no libm and no fused multiply-add.
 * The result is the correctly rounded float (nearest even) for every finite x >= 0 and y > 0
 * the lighting passes, subnormal results included; tools/test_shader_embed.py keeps the copies
 * identical and the tnl-parity selftest compares their output. Fixed-point values are
 * (high word, low word) pairs. ge_pow_mul is the high 64 bits of a 64 x 64-bit product:
 * ge_pow_mul_limbs here, or the same product from a 128-bit multiply where C has one. */
GE_POW_FN uvec2 ge_pow_add(uvec2 a, uvec2 b) {
    uint lo = a.y + b.y;
    return ge_v2(a.x + b.x + (lo < a.y ? 1u : 0u), lo);
}
GE_POW_FN uvec2 ge_pow_sub(uvec2 a, uvec2 b) {
    return ge_v2(a.x - b.x - (a.y < b.y ? 1u : 0u), a.y - b.y);
}
GE_POW_FN uvec2 ge_pow_mul_limbs(uvec2 a, uvec2 b) {
    uint t = ge_pow_mulhi(a.y, b.y);
    uint u = t + a.y * b.x;
    uint k = u < t ? 1u : 0u;
    uint v = u + a.x * b.y;
    k = k + (v < u ? 1u : 0u);
    uint d = ge_pow_mulhi(a.y, b.x);
    uint e = d + ge_pow_mulhi(a.x, b.y);
    uint j = e < d ? 1u : 0u;
    uint f = e + a.x * b.x;
    j = j + (f < e ? 1u : 0u);
    uint g = f + k;
    j = j + (g < f ? 1u : 0u);
    return ge_v2(ge_pow_mulhi(a.x, b.x) + j, g);
}
/* ---- END ge_pow helpers ---- */
uvec2 ge_pow_mul(uvec2 a, uvec2 b) { return ge_pow_mul_limbs(a, b); }
/* ---- BEGIN ge_powf (shared text) ---- */
/* log2 x = xe + log2 m, m in [sqrt(1/2), sqrt(2)): GE_POW_R[i] is 1/m rounded up for the 92
 * intervals of m (i = mq >> 24), GE_POW_TH:TL[i] = -log2 R, and a = m R - 1 in [0, 1/90) is
 * exact; a six-term series gives log2(1 + a). |y log2 x| is formed in 16.48 fixed point and
 * split as n + g, and 2^g = 2^(j/64) 2^d with GE_POW_E[j] and a five-term series in d. A NaN
 * x or y is returned as is, a negative x or y gives NaN, x^0 is 1 and 0^y is +0. */
GE_POW_FN float ge_powf(float x, float y) {
    uint xb = floatBitsToUint(x);
    uint yb = floatBitsToUint(y);
    if ((xb & 0x7FFFFFFFu) > 0x7F800000u) return x;
    if ((yb & 0x7FFFFFFFu) > 0x7F800000u) return y;
    if ((yb & 0x7FFFFFFFu) == 0u) return 1.0f;
    if (yb > 0x7FFFFFFFu) return uintBitsToFloat(0x7FC00000u);
    if ((xb & 0x7FFFFFFFu) == 0u) return 0.0f;
    if (xb > 0x7FFFFFFFu) return uintBitsToFloat(0x7FC00000u);
    if (xb == 0x3F800000u) return 1.0f;
    if (xb == 0x7F800000u || yb == 0x7F800000u)
        return xb > 0x3F800000u ? uintBitsToFloat(0x7F800000u) : 0.0f;
    /* x = xm * 2^(xe - 23) and y = ym * 2^(ye - 23), xm and ym in [2^23, 2^24). */
    int xe = ge_i((xb >> 23) & 0xFFu);
    uint xm = xb & 0x7FFFFFu;
    if (xe == 0) { int sh = 23 - findMSB(xm); xm = xm << sh; xe = 1 - sh; }
    else xm = xm | 0x800000u;
    xe = xe - 127;
    int ye = ge_i((yb >> 23) & 0xFFu);
    uint ym = yb & 0x7FFFFFu;
    if (ye == 0) { int sh = 23 - findMSB(ym); ym = ym << sh; ye = 1 - sh; }
    else ym = ym | 0x800000u;
    ye = ye - 127;
    /* x = m * 2^xe, m as mq in 1.31 fixed point. */
    uint mq = xm << 8;
    if (xm > 0xB504F3u) { mq = xm << 7; xe = xe + 1; }
    uint ix = (mq >> 24) - 90u;
    /* a = m R - 1 in 0.64 fixed point. */
    uint ph = ge_pow_mulhi(mq, GE_POW_R[ix]);
    uint pl = mq * GE_POW_R[ix];
    uvec2 a = ge_v2(((ph - 0x40000000u) << 2) | (pl >> 30), pl << 2);
    /* log2(1 + a) = a (1/ln 2 - a (1/(2 ln 2) - a (... - a / (6 ln 2)))), coefficients 1.63. */
    uvec2 b = ge_pow_sub(ge_v2(0x24EED8A1u, 0xDF37FCF2u), ge_pow_mul(a, ge_v2(0x1EC709DCu, 0x3A03FD75u)));
    b = ge_pow_sub(ge_v2(0x2E2A8ECAu, 0x5705FC2Fu), ge_pow_mul(a, b));
    b = ge_pow_sub(ge_v2(0x3D8E13B8u, 0x7407FAE9u), ge_pow_mul(a, b));
    b = ge_pow_sub(ge_v2(0x5C551D94u, 0xAE0BF85Eu), ge_pow_mul(a, b));
    b = ge_pow_sub(ge_v2(0xB8AA3B29u, 0x5C17F0BCu), ge_pow_mul(a, b));
    /* log2 m = log2(1 + a) - log2 R, 1.63 two's complement. */
    uvec2 l = ge_pow_add(ge_v2(GE_POW_TH[ix], GE_POW_TL[ix]), ge_pow_mul(a, b));
    /* |log2 x| = |xe + log2 m| as l * 2^-fb, shifted to its top bit; lneg its sign. */
    int fb = 63;
    if (xe != 0) {
        l = ge_v2((l.x >> 9) | ((l.x & 0x80000000u) != 0u ? 0xFF800000u : 0u), (l.y >> 9) | (l.x << 23));
        l.x = l.x + (ge_u(xe) << 22);
        fb = 54;
    }
    uint lneg = l.x >> 31;
    if (lneg != 0u) l = ge_pow_sub(ge_v2(0u, 0u), l);
    if ((l.x | l.y) == 0u) return 1.0f;
    int nz = l.x != 0u ? 31 - findMSB(l.x) : 63 - findMSB(l.y);
    if (nz >= 32) l = ge_v2(l.y << (nz - 32), 0u);
    else if (nz > 0) l = ge_v2((l.x << nz) | (l.y >> (32 - nz)), l.y << nz);
    fb = fb + nz;
    /* |y log2 x| = w * 2^(9 + ye - fb), w = ym * l / 2^32, as p in 16.48 fixed point, held at
     * 512 once the result is 0 or infinite anyway. */
    uvec2 w = ge_pow_mul(ge_v2(ym, 0u), l);
    int s = 57 + ye - fb;
    uvec2 p = ge_v2(0x02000000u, 0u);
    if (s >= 0) {
        if (32 + findMSB(w.x) + s < 57)
            p = s == 0 ? w : ge_v2((w.x << s) | (w.y >> (32 - s)), w.y << s);
    } else if (s > -32) {
        p = ge_v2(w.x >> (-s), (w.y >> (-s)) | (w.x << (32 + s)));
    } else if (s > -64) {
        p = ge_v2(0u, s == -32 ? w.x : w.x >> (-32 - s));
    } else {
        p = ge_v2(0u, 0u);
    }
    /* y log2 x = n + g, g in [0, 1) as 0.48 fixed point (g.x holds its top 16 bits). */
    int n = ge_i(p.x >> 16);
    uvec2 g = ge_v2(p.x & 0xFFFFu, p.y);
    if (lneg != 0u) {
        n = -n;
        if ((g.x | g.y) != 0u) { n = n - 1; g = ge_pow_sub(ge_v2(0x10000u, 0u), g); }
    }
    if (n >= 128) return uintBitsToFloat(0x7F800000u);
    if (n <= -151) return 0.0f;
    /* 2^g = 2^(j/64) 2^d, d = g - j/64 in 0.64 fixed point, and
     * 2^d - 1 = d (ln 2 + d ((ln 2)^2/2 + d (... + d (ln 2)^5/120))), coefficients 0.64. */
    uint j = g.x >> 10;
    uvec2 d = ge_v2(((g.x & 0x3FFu) << 16) | (g.y >> 16), g.y << 16);
    b = ge_pow_add(ge_v2(0x0276556Du, 0xF749CEE5u), ge_pow_mul(d, ge_v2(0x005761FFu, 0x9E299CC4u)));
    b = ge_pow_add(ge_v2(0x0E35846Bu, 0x82505FC6u), ge_pow_mul(d, b));
    b = ge_pow_add(ge_v2(0x3D7F7BFFu, 0x058B1D51u), ge_pow_mul(d, b));
    b = ge_pow_add(ge_v2(0xB17217F7u, 0xD1CF79ACu), ge_pow_mul(d, b));
    b = ge_pow_mul(d, b);
    /* 2^(j/64) (1.63) times 1 + b (1.63): r in 2.62 fixed point, in [1, 2). */
    uvec2 r = ge_pow_mul(ge_v2(GE_POW_EH[j], GE_POW_EL[j]), ge_v2(0x80000000u | (b.x >> 1), (b.y >> 1) | (b.x << 31)));
    /* r * 2^(n - 62) to a float, rounded to nearest even. */
    int be = n + 127;
    if (be >= 1) {
        uint m = r.x >> 7;
        uint rem = r.x & 0x7Fu;
        if (rem > 0x40u || (rem == 0x40u && (r.y != 0u || (m & 1u) != 0u))) m = m + 1u;
        if (m >= 0x1000000u) { m = m >> 1; be = be + 1; }
        if (be >= 255) return uintBitsToFloat(0x7F800000u);
        return uintBitsToFloat((ge_u(be) << 23) | (m & 0x7FFFFFu));
    }
    int sh = -119 - n;
    uint sm = r.x >> sh;
    uint srem = r.x & ((1u << sh) - 1u);
    uint hb = 1u << (sh - 1);
    if (srem > hb || (srem == hb && (r.y != 0u || (sm & 1u) != 0u))) sm = sm + 1u;
    return uintBitsToFloat(sm);
}
/* ---- END ge_powf ---- */

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
        float msr = pf(P_MATSPE + 0), msg = pf(P_MATSPE + 1), msb = pf(P_MATSPE + 2);
        float spec = pf(P_SPECCOEF);
        if ((mupd & 1u) != 0u) { mar = vrf; mag = vgf; mab = vbf; }
        if ((mupd & 2u) != 0u) { mdr = vrf; mdg = vgf; mdb = vbf; }
        if ((mupd & 4u) != 0u) { msr = vrf; msg = vgf; msb = vbf; }
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
            uint comp = pu(l + 14);
            precise float ndl = wnx * lx + wny * ly + wnz * lz;
            if (ndl < 0.0) ndl = 0.0;
            if (comp == 2u && spec > 0.0) ndl = ge_powf(ndl, spec);   /* powered diffuse */
            precise float addr = att * (pf(l + 5) * mar + pf(l + 8) * mdr * ndl);
            precise float addg = att * (pf(l + 6) * mag + pf(l + 9) * mdg * ndl);
            precise float addb = att * (pf(l + 7) * mab + pf(l + 10) * mdb * ndl);
            lr = lr + addr; lg = lg + addg; lb = lb + addb;
            if ((comp & 1u) != 0u && ndl > 0.0) {   /* specular, Blinn half-vector with eye (0,0,1) */
                precise float hx = lx, hy = ly, hz = lz + 1.0;
                precise float hd2 = hx * hx + hy * hy + hz * hz;
                float hd = sqrt_rn(hd2);
                if (hd > 0.0) {
                    precise float nh = wnx * hx + wny * hy + wnz * hz;
                    float ndh = div_rn(nh, hd);
                    if (ndh > 0.0) {
                        float sp = spec > 0.0 ? ge_powf(ndh, spec) : ndh;
                        precise float spr = att * pf(l + 15) * msr * sp;
                        precise float spg = att * pf(l + 16) * msg * sp;
                        precise float spb = att * pf(l + 17) * msb * sp;
                        lr = lr + spr; lg = lg + spg; lb = lb + spb;
                    }
                }
            }
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
    v_uvbox = vec4(1.0, 1.0, 0.0, 0.0);   /* transform mode: no edge clamp (psp.vert) */
}
