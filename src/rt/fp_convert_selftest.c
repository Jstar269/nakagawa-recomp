// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// Fixed-vector regression for the canonical scalar-FPU/VFPU conversion layer.
// Expected words are source-owned constants, not results from another helper.

#include "fp_convert.h"

#include <fenv.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

typedef struct ScalarVector {
    const char *name;
    uint32_t input_bits;
    uint32_t funct;
    uint32_t fcr31;
    uint32_t expected;
} ScalarVector;

typedef struct VfpuVector {
    const char *name;
    uint32_t input_bits;
    unsigned mode;
    unsigned scale;
    uint32_t expected;
} VfpuVector;

static int g_checks;
static int g_failures;

static float float_from_bits(uint32_t bits) {
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

static void check_word(const char *name, const char *host_mode,
                       uint32_t got, uint32_t expected) {
    g_checks++;
    if (got != expected) {
        fprintf(stderr, "FAIL: %s under %s: got 0x%08x, expected 0x%08x\n",
                name, host_mode, got, expected);
        g_failures++;
    }
}

static const ScalarVector kScalarVectors[] = {
    {"round +0",              0x00000000u, 0x0cu, 0u, 0x00000000u},
    {"round -0",              0x80000000u, 0x0cu, 0u, 0x00000000u},
    {"round +subnormal",      0x00000001u, 0x0cu, 0u, 0x00000000u},
    {"round -subnormal",      0x80000001u, 0x0cu, 0u, 0x00000000u},
    {"round 0.5 ties even",   0x3f000000u, 0x0cu, 0u, 0x00000000u},
    {"round 1.5 ties even",   0x3fc00000u, 0x0cu, 0u, 0x00000002u},
    {"round 2.5 ties even",   0x40200000u, 0x0cu, 0u, 0x00000002u},
    {"round 3.5 ties even",   0x40600000u, 0x0cu, 0u, 0x00000004u},
    {"round -1.5 ties even",  0xbfc00000u, 0x0cu, 0u, 0xfffffffeu},
    {"round -2.5 ties even",  0xc0200000u, 0x0cu, 0u, 0xfffffffeu},
    {"trunc 2.75",            0x40300000u, 0x0du, 0u, 0x00000002u},
    {"trunc -2.75",           0xc0300000u, 0x0du, 0u, 0xfffffffeu},
    {"ceil 2.25",             0x40100000u, 0x0eu, 0u, 0x00000003u},
    {"ceil -2.25",            0xc0100000u, 0x0eu, 0u, 0xfffffffeu},
    {"floor 2.75",            0x40300000u, 0x0fu, 0u, 0x00000002u},
    {"floor -2.25",           0xc0100000u, 0x0fu, 0u, 0xfffffffdu},
    {"cvt RN 2.5",            0x40200000u, 0x24u, 0u, 0x00000002u},
    {"cvt RZ -2.75",          0xc0300000u, 0x24u, 1u, 0xfffffffeu},
    {"cvt RP 2.25",           0x40100000u, 0x24u, 2u, 0x00000003u},
    {"cvt RM -2.25",          0xc0100000u, 0x24u, 3u, 0xfffffffdu},
    {"cvt masks FCR31",       0x40300000u, 0x24u, 0xfffffffdu, 0x00000002u},
    {"largest below INTMAX",  0x4effffffu, 0x0du, 0u, 0x7fffff80u},
    {"exact +2^31",           0x4f000000u, 0x0du, 0u, 0x7fffffffu},
    {"exact INT32_MIN",       0xcf000000u, 0x0du, 0u, 0x80000000u},
    {"below INT32_MIN",       0xcf000001u, 0x0du, 0u, 0x80000000u},
    {"positive FLT_MAX",      0x7f7fffffu, 0x0cu, 0u, 0x7fffffffu},
    {"negative FLT_MAX",      0xff7fffffu, 0x0cu, 0u, 0x80000000u},
    {"positive infinity",     0x7f800000u, 0x0eu, 0u, 0x7fffffffu},
    {"negative infinity",     0xff800000u, 0x0fu, 0u, 0x80000000u},
    {"positive quiet NaN",    0x7fc00000u, 0x0du, 0u, 0x7fffffffu},
    {"negative quiet NaN",    0xffc00000u, 0x24u, 3u, 0x7fffffffu},
    {"positive signaling NaN",0x7f800001u, 0x0cu, 0u, 0x7fffffffu},
    {"negative signaling NaN",0xff800001u, 0x0fu, 0u, 0x7fffffffu},
};

static const VfpuVector kVfpuVectors[] = {
    {"vf2in 0.5",          0x3f000000u, 0u, 0u, 0x00000000u},
    {"vf2in 1.5",          0x3fc00000u, 0u, 0u, 0x00000002u},
    {"vf2in 2.5",          0x40200000u, 0u, 0u, 0x00000002u},
    {"vf2in -1.5",         0xbfc00000u, 0u, 0u, 0xfffffffeu},
    {"vf2iz -2.75",        0xc0300000u, 1u, 0u, 0xfffffffeu},
    {"vf2iu -2.25",        0xc0100000u, 2u, 0u, 0xfffffffeu},
    {"vf2id -2.25",        0xc0100000u, 3u, 0u, 0xfffffffdu},
    {"vf2in scaled tie",   0x3f400000u, 0u, 1u, 0x00000002u},
    {"vf2iz scale 31",     0x3f7fffffu, 1u, 31u, 0x7fffff80u},
    {"vf2in +overflow",    0x3f800000u, 0u, 31u, 0x7fffffffu},
    {"vf2in -boundary",    0xbf800000u, 0u, 31u, 0x80000000u},
    {"vf2in +infinity",    0x7f800000u, 0u, 0u, 0x7fffffffu},
    {"vf2in -infinity",    0xff800000u, 0u, 0u, 0x80000000u},
    {"vf2in NaN",          0x7fc00000u, 0u, 0u, 0x7fffffffu},
    {"vf2in signaling NaN",0x7f800001u, 0u, 0u, 0x7fffffffu},
    {"vf2iu subnormal",    0x00000001u, 2u, 31u, 0x00000001u},
};

/* Guest-RM-directed scalar arithmetic. Expected words are source-owned,
 * independently derived correctly-rounded binary32 results; the mul.s RM row
 * reproduces the PSP_HARDWARE-published anchors (18.386576 RN/RP vs
 * 18.386574 RZ/RM) and the FS rows reproduce the measured flush gate on
 * smallest-normal*0.5. Helpers must dominate the ambient host rounding mode,
 * so every row runs under all four host modes. */
typedef struct ScalarOpVector {
    const char *name;
    uint8_t op;               /* 0=add 1=sub 2=mul 3=div */
    uint32_t a_bits;
    uint32_t b_bits;
    uint32_t fcr31;
    uint32_t expected;
} ScalarOpVector;

#define SR_FP_OP_ADD 0u
#define SR_FP_OP_SUB 1u
#define SR_FP_OP_MUL 2u
#define SR_FP_OP_DIV 3u

static const ScalarOpVector kScalarOpVectors[] = {
    {"mul RM anchors RN",   SR_FP_OP_MUL, 0x3e97d668u, 0x42780000u, 0u,          0x419317b5u},
    {"mul RM anchors RZ",   SR_FP_OP_MUL, 0x3e97d668u, 0x42780000u, 1u,          0x419317b4u},
    {"mul RM anchors RP",   SR_FP_OP_MUL, 0x3e97d668u, 0x42780000u, 2u,          0x419317b5u},
    {"mul RM anchors RM",   SR_FP_OP_MUL, 0x3e97d668u, 0x42780000u, 3u,          0x419317b4u},
    {"add tie RN keeps",    SR_FP_OP_ADD, 0x3f800000u, 0x33800000u, 0u,          0x3f800000u},
    {"add tie RP bumps",    SR_FP_OP_ADD, 0x3f800000u, 0x33800000u, 2u,          0x3f800001u},
    {"sub tie RP toward+inf",SR_FP_OP_SUB, 0x3f800000u, 0x33c00000u, 2u,          0x3f7fffffu},
    {"div 1/3 RN",          SR_FP_OP_DIV, 0x3f800000u, 0x40400000u, 0u,          0x3eaaaaabu},
    {"div 1/3 RZ",          SR_FP_OP_DIV, 0x3f800000u, 0x40400000u, 1u,          0x3eaaaaaau},
    {"mul FS=0 gradual",    SR_FP_OP_MUL, 0x00800000u, 0x3f000000u, 0u,          0x00400000u},
    {"mul FS=1 flush",      SR_FP_OP_MUL, 0x00800000u, 0x3f000000u, 0x01000000u, 0x00000000u},
    {"mul FS=1 flush -sign",SR_FP_OP_MUL, 0x80800000u, 0x3f000000u, 0x01000000u, 0x80000000u},
    {"mul FS=1 RN normal",  SR_FP_OP_MUL, 0x3e97d668u, 0x42780000u, 0x01000000u, 0x419317b5u},
    {"mul RM+FS combined",  SR_FP_OP_MUL, 0x3e97d668u, 0x42780000u, 0x01000001u, 0x419317b4u},
    {"mul subnormal input exact", SR_FP_OP_MUL, 0x00000001u, 0x40000000u, 0u, 0x00000002u},
};

/* CVT.S.W honors guest RM near the binary32 precision boundary. */
typedef struct CvtSwVector {
    const char *name;
    uint32_t word;
    uint32_t fcr31;
    uint32_t expected;
} CvtSwVector;

static const CvtSwVector kCvtSwVectors[] = {
    {"cvt.s.w 2^24+1 RN",   0x01000001u, 0u,          0x4b800000u},
    {"cvt.s.w 2^24+1 RP",   0x01000001u, 2u,          0x4b800001u},
    {"cvt.s.w 2^24+1 RZ",   0x01000001u, 1u,          0x4b800000u},
    {"cvt.s.w -(2^24+1) RN",0xfeffffffu, 0u,          0xcb800000u},
    {"cvt.s.w -(2^24+1) RM",0xfeffffffu, 3u,          0xcb800001u},
    {"cvt.s.w exact 7",     0x00000007u, 2u,          0x40e00000u},
    {"cvt.s.w exact -1",    0xffffffffu, 1u,          0xbf800000u},
};

static uint32_t apply_scalar_op(unsigned op, float a, float b, uint32_t fcr31) {
    float r;
    switch (op) {
        case SR_FP_OP_ADD: r = sr_fpu_add_s(a, b, fcr31); break;
        case SR_FP_OP_SUB: r = sr_fpu_sub_s(a, b, fcr31); break;
        case SR_FP_OP_MUL: r = sr_fpu_mul_s(a, b, fcr31); break;
        default:           r = sr_fpu_div_s(a, b, fcr31); break;
    }
    return sr_float_bits(r);
}


int main(void) {
    static const struct {
        const char *name;
        int value;
    } host_modes[] = {
        {"FE_TONEAREST", FE_TONEAREST},
        {"FE_DOWNWARD", FE_DOWNWARD},
        {"FE_UPWARD", FE_UPWARD},
        {"FE_TOWARDZERO", FE_TOWARDZERO},
    };
    const int saved_mode = fegetround();

    /* Host environment baseline, captured before any helper runs so the
     * restoration probe can prove exact recovery rather than assume defaults. */
    const uint32_t csr_baseline = sr_fpu_env_save();
    float baseline_third = 1.0f / 3.0f;
    const uint32_t baseline_third_bits = sr_float_bits(baseline_third);

    for (size_t m = 0; m < sizeof(host_modes) / sizeof(host_modes[0]); m++) {
        if (fesetround(host_modes[m].value) != 0) {
            fprintf(stderr, "FAIL: host does not support %s\n", host_modes[m].name);
            g_failures++;
            continue;
        }
        for (size_t i = 0; i < sizeof(kScalarVectors) / sizeof(kScalarVectors[0]); i++) {
            const ScalarVector *v = &kScalarVectors[i];
            check_word(v->name, host_modes[m].name,
                       sr_fpu_to_word(float_from_bits(v->input_bits), v->funct, v->fcr31),
                       v->expected);
        }
        for (size_t i = 0; i < sizeof(kVfpuVectors) / sizeof(kVfpuVectors[0]); i++) {
            const VfpuVector *v = &kVfpuVectors[i];
            check_word(v->name, host_modes[m].name,
                       sr_vfpu_to_word(float_from_bits(v->input_bits), v->mode, v->scale),
                       v->expected);
        }
        /* Guest-RM arithmetic must dominate whatever mode the host is in. */
        for (size_t i = 0; i < sizeof(kScalarOpVectors) / sizeof(kScalarOpVectors[0]); i++) {
            const ScalarOpVector *v = &kScalarOpVectors[i];
            check_word(v->name, host_modes[m].name,
                       apply_scalar_op(v->op, float_from_bits(v->a_bits), float_from_bits(v->b_bits), v->fcr31),
                       v->expected);
        }
        for (size_t i = 0; i < sizeof(kCvtSwVectors) / sizeof(kCvtSwVectors[0]); i++) {
            const CvtSwVector *v = &kCvtSwVectors[i];
            char label[128];
            snprintf(label, sizeof(label), "%s under %s", v->name, host_modes[m].name);
            check_word(label, "host-mode",
                       sr_float_bits(sr_fpu_cvt_s_w(sr_u32_as_s32(v->word), v->fcr31)),
                       v->expected);
        }
    }
    if (saved_mode != -1) (void)fesetround(saved_mode);

    check_word("signed bits zero", "representation", (uint32_t)sr_u32_as_s32(0u), 0u);
    check_word("signed bits INTMAX", "representation", (uint32_t)sr_u32_as_s32(0x7fffffffu), 0x7fffffffu);
    check_word("signed bits INTMIN", "representation", (uint32_t)sr_u32_as_s32(0x80000000u), 0x80000000u);
    check_word("signed bits -1", "representation", (uint32_t)sr_u32_as_s32(0xffffffffu), 0xffffffffu);

    /* Fast-path predicate removed for correctness (see fp_convert.h, FAST
     * PATH DISPOSITION): the hostile-host matrix below is the standing proof
     * that default-state guests are isolated without it. */

    /* Host-environment hygiene (RISK-8): every helper must return the host FP
     * control word exactly as found, and a later native operation must behave
     * as it did before any helper ran. */
    {
        (void)sr_fpu_mul_s(float_from_bits(0x3e97d668u), float_from_bits(0x42780000u),
                           SR_FCR31_FS | 1u);
        const uint32_t csr_after = sr_fpu_env_save();
        g_checks++;
        if (csr_after != csr_baseline) {
            fprintf(stderr, "FAIL: helper left host FP env modified (before=0x%08x after=0x%08x)\n",
                    csr_baseline, csr_after);
            g_failures++;
        }
        float gradual = float_from_bits(0x00800000u) * float_from_bits(0x3f000000u);
        check_word("native gradual underflow survives helpers", "restoration",
                   sr_float_bits(gradual), 0x00400000u);
        float third = 1.0f / 3.0f;
        check_word("native rounding unchanged by helpers", "restoration",
                   sr_float_bits(third), baseline_third_bits);
    }

    /* Non-finite inputs pass through as NaN results under a directed mode;
     * payload selection is host-dependent and deliberately unasserted. */
    {
        uint32_t got = apply_scalar_op(SR_FP_OP_MUL, float_from_bits(0x7fc00001u),
                                       float_from_bits(0x3f800000u), 1u);
        g_checks++;
        if ((got & 0x7f800000u) != 0x7f800000u || (got & 0x007fffffu) == 0u) {
            fprintf(stderr, "FAIL: NaN input did not produce a NaN result (0x%08x)\n", got);
            g_failures++;
        }
    }

    /* Hostile host environment: a foreign library may have left MXCSR.DAZ set
     * (RISK-8). The modeled contract gives subnormal INPUTS gradual weight
     * regardless of guest FS, so a helper entered with ambient DAZ=1 must
     * still weight the input exactly, and must restore the caller's
     * environment bit-for-bit -- including that DAZ bit itself. */
    {
        const uint32_t csr_base = sr_fpu_env_save();
        const uint32_t csr_hostile = csr_base | (1u << 6);   /* MXCSR.DAZ */
        _mm_setcsr(csr_hostile);
        const uint32_t got = apply_scalar_op(SR_FP_OP_MUL,
                                             float_from_bits(0x00000001u),  /* min subnormal */
                                             float_from_bits(0x40000000u),  /* exactly 2.0   */
                                             0u);
        check_word("subnormal input keeps weight under hostile DAZ", "hostile-env",
                   got, 0x00000002u);
        g_checks++;
        const uint32_t csr_after = sr_fpu_env_save();
        if (csr_after != csr_hostile) {
            fprintf(stderr, "FAIL: hostile-env restoration inexact (caller=0x%08x after=0x%08x)\n",
                    csr_hostile, csr_after);
            g_failures++;
        }
        _mm_setcsr(csr_base);   /* leave the process environment as found */
    }

    /* Full hostile-host matrix: every ambient control field that could bias a
     * guest operation -- rounding mode, FTZ, DAZ, sticky status bits, and
     * exception masks (including fully unmasked, which an unhardened window
     * would let escalate into a host FP fault). Each row enters helpers under
     * the mutated environment with GUEST RM/FS at defaults and requires both
     * correct guest result bits and exact caller-environment restoration. */
    {
        struct HostileBase { const char *name; uint32_t set; uint32_t clear; };
        static const struct HostileBase bases[] = {
            {"RC=RZ",            1u << 13,       0u},
            {"RC=RP",            2u << 13,       0u},
            {"RC=RM",            3u << 13,       0u},
            {"FTZ",              1u << 15,       0u},
            {"DAZ",              1u << 6,        0u},
            {"sticky PE|UE|OE",  0x32u,          0u},
            {"fully unmasked",   0u,             0x1f80u},
            {"combined hostile", (3u << 13) | (1u << 15) | (1u << 6) | 0x32u, 0x1f80u},
        };
        struct GuestCase { const char *name; unsigned op; uint32_t a, b, fcr31, want; };
        static const struct GuestCase cases[] = {
            {"mul gradual out", SR_FP_OP_MUL, 0x00800000u, 0x3f000000u, 0x00000000u, 0x00400000u},
            {"mul subnormal in", SR_FP_OP_MUL, 0x00000001u, 0x40000000u, 0x00000000u, 0x00000002u},
            {"div 1/3 RZ guest", SR_FP_OP_DIV, 0x3f800000u, 0x40400000u, 0x00000001u, 0x3eaaaaaau},
        };
        const uint32_t csr_base = sr_fpu_env_save();
        char label[128];
        for (size_t bi = 0; bi < sizeof(bases) / sizeof(bases[0]); bi++) {
            const uint32_t hostile = (csr_base | bases[bi].set) & ~bases[bi].clear;
            _mm_setcsr(hostile);
            for (size_t ci = 0; ci < sizeof(cases) / sizeof(cases[0]); ci++) {
                snprintf(label, sizeof(label), "%s under %s", cases[ci].name, bases[bi].name);
                check_word(label, "hostile-matrix",
                           apply_scalar_op(cases[ci].op,
                                           float_from_bits(cases[ci].a),
                                           float_from_bits(cases[ci].b),
                                           cases[ci].fcr31),
                           cases[ci].want);
            }
            snprintf(label, sizeof(label), "cvt RP guest under %s", bases[bi].name);
            check_word(label, "hostile-matrix",
                       sr_float_bits(sr_fpu_cvt_s_w(16777217, 2u)),
                       0x4b800001u);
            g_checks++;
            const uint32_t csr_after = sr_fpu_env_save();
            if (csr_after != hostile) {
                fprintf(stderr, "FAIL: hostile-matrix restoration under %s "
                                "(caller=0x%08x after=0x%08x)\n",
                        bases[bi].name, hostile, csr_after);
                g_failures++;
            }
        }
        _mm_setcsr(csr_base);   /* leave the process environment as found */
    }

    /* Compile-time folding guard: operands supplied as direct expressions are
     * fully visible to the optimizer, so a helper whose arithmetic could be
     * constant-folded under the abstract default rounding mode would return
     * RN bits here instead of the directed ones. These checks only mean
     * something when this TU is built optimized; the CI/local gate matrix
     * builds it at multiple optimization levels for exactly that reason. */
    check_word("literal mul RZ not folded to RN", "folding-guard",
               sr_float_bits(sr_fpu_mul_s(float_from_bits(0x3e97d668u),
                                          float_from_bits(0x42780000u), 1u)),
               0x419317b4u);
    check_word("literal add RP not folded to RN", "folding-guard",
               sr_float_bits(sr_fpu_add_s(float_from_bits(0x3f800000u),
                                          float_from_bits(0x33800000u), 2u)),
               0x3f800001u);
    check_word("literal div RZ not folded to RN", "folding-guard",
               sr_float_bits(sr_fpu_div_s(float_from_bits(0x3f800000u),
                                          float_from_bits(0x40400000u), 1u)),
               0x3eaaaaaau);
    check_word("literal cvt.s.w RP not folded to RN", "folding-guard",
               sr_float_bits(sr_fpu_cvt_s_w(16777217, 2u)),
               0x4b800001u);

    if (g_failures != 0) {
        fprintf(stderr, "fp_convert_selftest: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("fp_convert_selftest: all %d fixed-vector checks passed\n", g_checks);
    return 0;
}
