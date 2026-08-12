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
    }
    if (saved_mode != -1) (void)fesetround(saved_mode);

    check_word("signed bits zero", "representation", (uint32_t)sr_u32_as_s32(0u), 0u);
    check_word("signed bits INTMAX", "representation", (uint32_t)sr_u32_as_s32(0x7fffffffu), 0x7fffffffu);
    check_word("signed bits INTMIN", "representation", (uint32_t)sr_u32_as_s32(0x80000000u), 0x80000000u);
    check_word("signed bits -1", "representation", (uint32_t)sr_u32_as_s32(0xffffffffu), 0xffffffffu);

    if (g_failures != 0) {
        fprintf(stderr, "fp_convert_selftest: %d/%d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("fp_convert_selftest: all %d fixed-vector checks passed\n", g_checks);
    return 0;
}
