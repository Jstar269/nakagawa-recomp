#version 450
// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)
// Modified by Nakagawa Recomp contributors, 2026-08-10.
// See NOTICE.md for upstream lineage and modification provenance.

/* PSP GE fragment stage uber-shader, Phase 1. Mirrors ge.c shade() + zrange_ok():
 * texfunc 0-7 (modulate/decal/blend/replace/add) with the RGBA and color-double bits,
 * programmable alpha test, fog mix, and the minz/maxz depth-range DISCARD (transform
 * mode only -- it is a kill test on the PSP, not a clamp). Clear-mode draws bypass
 * texturing/alpha-test/fog like put_px_rgba does. Simple blends and write masks are VK
 * fixed-function state chosen by ge_gpu.c; blend states fixed-function Vulkan cannot
 * express (doubled DST-alpha factors, absdiff, two distinct FIX constants, post-blend
 * 16-bit quantization/dither) run HERE against a destination snapshot (u_dst, taken at
 * batch-build time -- overlapping primitives inside one batch read the pre-batch
 * destination, the standard shader-blend hazard). Colors are computed in 0..255 space
 * to track the integer formulas in shade()/blend_chan(). */
layout(location = 0) noperspective in vec2  v_uv;
layout(location = 1) noperspective in float v_rw;
layout(location = 2) noperspective in float v_fog;
layout(location = 3) noperspective in vec4  v_color;

layout(location = 0) out vec4 o_color;

layout(set = 0, binding = 0) uniform sampler2D u_tex;
layout(set = 0, binding = 1) uniform sampler2D u_dst;   /* destination snapshot (shader blend) */

/* flags bits (pc.cfg.x >> 8): 1 textured, 2 texfunc-RGBA, 4 color-double, 8 fog,
 * 16 persp (apply minz/maxz), 32 clear mode, 64 nearest-filter (+0.5 texel shift),
 * 128 premultiply rgb by 2*alpha (PSP src blend factor DOUBLE_SRC_ALPHA with VK factor
 * ONE), 256 premultiply rgb by (1 - 2*alpha) (ONE_MINUS_DOUBLE_SRC_ALPHA),
 * 512 ordered dithering enabled, bits 10-11 = framebuffer pixel format (psm 0..3):
 * the output color is snapped to the native 5650/5551/4444 lattice so the RGBA8
 * attachment only ever holds values that round-trip guest VRAM bit-exactly
 * (truncate(bit_replicate(x)) == x -> zero drift across readback/upload cycles),
 * 4096 shader blend (full PSP blend against u_dst; pipeline blending is OFF). */
layout(push_constant) uniform PC {
    ivec4 cfg;      /* x = texfunc | flags<<8, y = alpha test (func|ref<<8|mask<<16),
                       z = minz, w = maxz */
    vec4  texenv;   /* rgb = GE_TEXENVCOLOR / 255 */
    vec4  fogcol;   /* rgb = GE_FOGCOLOR / 255 */
    vec4  texsize;  /* xy = tex dimensions, zw = texel offset (render-target sub-rect) */
    ivec4 dith;     /* 4x4 GE dither matrix, one row per component, 4 x s8 packed per int */
    ivec4 bl;       /* x = blend srcf | dstf<<8 | eq<<16, y = render scale (>= 1) */
    vec4  fixa;     /* rgb = GE_BLENDFIXEDA / 255 (src FIX constant) */
    vec4  fixb;     /* rgb = GE_BLENDFIXEDB / 255 (dst FIX constant) */
} pc;

/* Keep the top `bits` of an 8-bit channel and expand by bit replication — exactly
 * ge.c's pack565/pack5551/pack4444 followed by unpack_color. */
float quant_chan(float c, int bits) {
    int v = int(c) >> (8 - bits);
    return float((v << (8 - bits)) | (v >> (2 * bits - 8)));
}

/* PSP blend factor, integer 0..255 space — exactly ge.c factor_component(). */
ivec3 blend_factor(int f, bool srcSide, ivec3 s, int sa, ivec3 d, int da) {
    switch (f & 15) {
        case 0:  return srcSide ? d : s;
        case 1:  return 255 - (srcSide ? d : s);
        case 2:  return ivec3(sa);
        case 3:  return ivec3(255 - sa);
        case 4:  return ivec3(da);
        case 5:  return ivec3(255 - da);
        case 6:  return ivec3(clamp(sa * 2, 0, 255));
        case 7:  return ivec3(clamp(255 - sa * 2, 0, 255));
        case 8:  return ivec3(clamp(da * 2, 0, 255));
        case 9:  return ivec3(clamp(255 - da * 2, 0, 255));
        default: return ivec3((srcSide ? pc.fixa.rgb : pc.fixb.rgb) * 255.0 + 0.5);
    }
}

void main() {
    int flags = pc.cfg.x >> 8;

    /* PSP depth-range test: discard transform-mode pixels outside [minz,maxz]. */
    if ((flags & 16) != 0) {
        int z16 = int(gl_FragCoord.z * 65535.0 + 0.5);
        if (z16 < pc.cfg.z || z16 > pc.cfg.w) discard;
    }

    vec4 v = v_color * 255.0;
    vec4 col = v;

    if ((flags & 32) == 0) {
        if ((flags & 1) != 0) {
            /* Recover texel coords (u*rw interpolated affinely, divided per pixel; in
             * through mode rw==1). Nearest filtering matches (int)(u+0.5) via the shift. */
            float rw = max(abs(v_rw), 1e-20);
            vec2 uv = v_uv / rw;
            if ((flags & 64) != 0) uv += vec2(0.5);
            uv += pc.texsize.zw;
            vec4 t = texture(u_tex, uv / pc.texsize.xy) * 255.0;
            int  tf    = pc.cfg.x & 7;
            bool rgba  = (flags & 2) != 0;
            float dscl = ((flags & 4) != 0) ? 2.0 : 1.0;
            vec3 rgb; float al;
            if (tf == 1) {                       /* decal */
                if (rgba) rgb = mix(v.rgb, t.rgb, t.a * (1.0 / 255.0)) * dscl;
                else      rgb = t.rgb * dscl;
                al = v.a;
            } else if (tf == 2) {                /* blend against TEXENVCOLOR */
                rgb = mix(v.rgb, pc.texenv.rgb * 255.0, t.rgb * (1.0 / 255.0)) * dscl;
                al  = rgba ? v.a * t.a * (1.0 / 255.0) : v.a;
            } else if (tf == 3) {                /* replace */
                rgb = t.rgb * dscl;
                al  = rgba ? t.a : v.a;
            } else if (tf >= 4) {                /* add */
                rgb = (v.rgb + t.rgb) * dscl;
                al  = rgba ? v.a * t.a * (1.0 / 255.0) : v.a;
            } else {                             /* modulate */
                rgb = v.rgb * t.rgb * (1.0 / 255.0) * dscl;
                al  = rgba ? v.a * t.a * (1.0 / 255.0) : v.a;
            }
            col = vec4(min(rgb, vec3(255.0)), min(al, 255.0));
        }

        /* Alpha test (func order matches ge.c alpha_test). */
        int at = pc.cfg.y, afunc = at & 7;
        if (afunc != 1) {
            int amask = (at >> 16) & 0xFF;
            int aref  = ((at >> 8) & 0xFF) & amask;
            int av    = int(col.a + 0.5) & amask;
            bool pass;
            if      (afunc == 0) pass = false;
            else if (afunc == 2) pass = (av == aref);
            else if (afunc == 3) pass = (av != aref);
            else if (afunc == 4) pass = (av <  aref);
            else if (afunc == 5) pass = (av <= aref);
            else if (afunc == 6) pass = (av >  aref);
            else                 pass = (av >= aref);
            if (!pass) discard;
        }

        if ((flags & 8) != 0) {
            float f = clamp(v_fog, 0.0, 1.0);   /* 1 = no fog (PPSSPP convention) */
            col.rgb = mix(pc.fogcol.rgb * 255.0, col.rgb, f);
        }

        /* doubled-src-alpha blend factors folded into the source color */
        if ((flags & 128) != 0) col.rgb *= clamp(col.a * 2.0, 0.0, 255.0) * (1.0 / 255.0);
        if ((flags & 256) != 0) col.rgb *= clamp(255.0 - col.a * 2.0, 0.0, 255.0) * (1.0 / 255.0);
    }

    /* Shader blend: full PSP factor/equation set against the destination snapshot,
     * integer math identical to ge.c blend_chan(). Runs BEFORE dither/quantization so
     * the 16-bit lattice snap applies to the blend RESULT, like the hardware store. */
    if ((flags & 4096) != 0) {
        ivec4 dpx = ivec4(texelFetch(u_dst, ivec2(gl_FragCoord.xy), 0) * 255.0 + 0.5);
        ivec3 s   = ivec3(col.rgb + 0.5);
        int   sa  = int(col.a + 0.5);
        int   sf  = pc.bl.x & 0xFF, df = (pc.bl.x >> 8) & 0xFF, eq = (pc.bl.x >> 16) & 7;
        ivec3 fs  = blend_factor(sf, true,  s, sa, dpx.rgb, dpx.a);
        ivec3 fd  = blend_factor(df, false, s, sa, dpx.rgb, dpx.a);
        ivec3 sv  = s * fs / 255, dv = dpx.rgb * fd / 255;
        ivec3 res;
        if      (eq == 1) res = clamp(sv - dv, 0, 255);
        else if (eq == 2) res = clamp(dv - sv, 0, 255);
        else if (eq == 3) res = min(s, dpx.rgb);        /* min/max/absdiff: factors ignored */
        else if (eq == 4) res = max(s, dpx.rgb);
        else if (eq == 5) res = abs(s - dpx.rgb);
        else              res = clamp(sv + dv, 0, 255);
        col.rgb = vec3(res);
    }

    /* PSP 4x4 ordered dither: same signed offset added to r/g/b, keyed by GUEST
     * framebuffer pixel position (ge.c compose_px_rgba) — divide the device coordinate
     * by the render scale so upscaled rendering keeps the native pattern. Applies in
     * clear mode too, and post-blend when shader blending is active (hardware order). */
    if ((flags & 512) != 0) {
        ivec2 fc = ivec2(gl_FragCoord.xy) / max(pc.bl.y, 1);
        int row = pc.dith[fc.y & 3];
        int dv = (row >> ((fc.x & 3) * 8)) & 0xFF;
        if (dv >= 128) dv -= 256;           /* sign-extend packed s8 */
        col.rgb = clamp(col.rgb + float(dv), 0.0, 255.0);
    }

    /* Native 16-bit store quantization (framebuffer psm 0/1/2). Emulates the precision
     * limit of the real 5650/5551/4444 framebuffer: pack by truncation, expand by bit
     * replication — identical to ge.c pack_fb + unpack_color, so the RGBA8 image and
     * guest VRAM stay bit-identical across arbitrarily many frames. */
    int fbfmt = (flags >> 10) & 3;
    if (fbfmt == 0) {
        col = vec4(quant_chan(col.r, 5), quant_chan(col.g, 6), quant_chan(col.b, 5), 255.0);
    } else if (fbfmt == 1) {
        col = vec4(quant_chan(col.r, 5), quant_chan(col.g, 5), quant_chan(col.b, 5),
                   col.a >= 128.0 ? 255.0 : 0.0);
    } else if (fbfmt == 2) {
        col = vec4(quant_chan(col.r, 4), quant_chan(col.g, 4), quant_chan(col.b, 4),
                   quant_chan(col.a, 4));
    }

    o_color = col * (1.0 / 255.0);
}
