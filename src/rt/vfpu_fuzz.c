// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
// Derived from sal063/PSP-recompilation-project (GPL-2.0-or-later)

/* * interpreter sr_vfpu_interp. For each VFPU compute word it runs many trials from identical
 * randomized CPU states (v[], prefixes, VFPU_CC) and compares the full v[] register file and
 * vfpuCtrl bitwise. Any divergence is a codegen bug of the vcmov class.
 *
 * The cases header is generated per game by tools/vfpu_fuzz_gen.py from every distinct VFPU
 * compute word in that game's ELF. Point this build at it with
 * -DVFPU_FUZZ_CASES='"path/to/vfpu_fuzz_cases.h"' (default: vfpu_fuzz_cases.h in the include path).
 *
 * Usage: vfpu_fuzz [trials_per_case] [seed]
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef VFPU_FUZZ_CASES
#define VFPU_FUZZ_CASES "vfpu_fuzz_cases.h"
#endif
#include VFPU_FUZZ_CASES

static uint32_t s_rng = 0x12345678u;
static uint32_t rng(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}

static float rand_float(void) {
    const char *constraint = getenv("FUZZ_CONSTRAINT");
    if (constraint && strcmp(constraint, "allow_nan_inf") == 0) {
        switch (rng() % 12) {
        case 0: return 0.0f;
        case 1: return 1.0f;
        case 2: return -1.0f;
        case 3: return 0.5f;
        case 4: { uint32_t nan = 0x7F800001; float f; memcpy(&f, &nan, 4); return f; }
        case 5: { uint32_t inf = 0x7F800000; float f; memcpy(&f, &inf, 4); return f; }
        case 6: { uint32_t ninf = 0xFF800000; float f; memcpy(&f, &ninf, 4); return f; }
        default: {
            int32_t m = (int32_t)(rng() % 4000) - 2000;
            return (float)m / 16.0f;
        }
        }
    } else if (constraint && strcmp(constraint, "positive") == 0) {
        switch (rng() % 8) {
        case 0: return 0.0f;
        case 1: return 1.0f;
        case 2: return 0.5f;
        default: {
            int32_t m = (int32_t)(rng() % 2000);
            return (float)m / 16.0f;
        }
        }
    } else if (constraint && strcmp(constraint, "no_zero") == 0) {
        switch (rng() % 7) {
        case 0: return 1.0f;
        case 1: return -1.0f;
        case 2: return 0.5f;
        default: {
            int32_t m = (int32_t)(rng() % 4000) - 2000;
            if (m == 0) m = 1;
            return (float)m / 16.0f;
        }
        }
    }

    switch (rng() % 8) {
    case 0: return 0.0f;
    case 1: return 1.0f;
    case 2: return -1.0f;
    case 3: return 0.5f;
    default: {
        /* modest finite values; both sides share the float kernels, so exotic inputs
         * (inf/NaN) would only test code both paths inherit from the same helpers */
        int32_t m = (int32_t)(rng() % 4000) - 2000;
        return (float)m / 16.0f;
    }
    }
}

static uint32_t rand_sprefix(void) {
    if (rng() % 2) return 0xe4;                 /* identity half the time */
    uint32_t p = 0;
    for (int i = 0; i < 4; i++) p |= (rng() & 3u) << (i * 2);   /* swizzle */
    p |= (rng() & 0xFu) << 8;                   /* abs bits */
    if (rng() % 4 == 0) p |= (rng() & 0xFu) << 12;  /* constant bits */
    p |= (rng() & 0xFu) << 16;                  /* negate bits */
    return p;
}

static uint32_t rand_dprefix(void) {
    if (rng() % 2) return 0;
    uint32_t p = 0;
    for (int i = 0; i < 4; i++) {
        uint32_t sat = rng() & 3u;
        if (sat == 2) sat = 0;                  /* 2 is reserved */
        p |= sat << (i * 2);
    }
    p |= (rng() & 0xFu) << 8;                   /* write mask */
    return p;
}

static int check_transcendentals(void) {
    float trig_err=0.0f,asin_err=0.0f,log_err=0.0f,sqrt_rel=0.0f;
    for(int i=-256;i<=256;i++){
        float x=(float)i/64.0f;
        float es=fabsf(sr_vfpu_sin(x)-sinf(x*1.57079632679489661923f));
        float ec=fabsf(sr_vfpu_cos(x)-cosf(x*1.57079632679489661923f));
        if(es>trig_err)trig_err=es;if(ec>trig_err)trig_err=ec;
    }
    for(int i=-1000;i<=1000;i++){
        float x=(float)i/1000.0f;
        float e=fabsf(sr_vfpu_asin(x)-asinf(x)/1.57079632679489661923f);
        if(e>asin_err)asin_err=e;
    }
    for(int i=1;i<=4096;i++){
        float x=(float)i/53.0f;
        float e=fabsf(sr_vfpu_log2(x)-log2f(x));if(e>log_err)log_err=e;
        float ref=sqrtf(x),got=sr_vfpu_sqrt(x);e=fabsf(got-ref)/ref;if(e>sqrt_rel)sqrt_rel=e;
        ref=1.0f/ref;got=sr_vfpu_rsqrt(x);e=fabsf(got-ref)/ref;if(e>sqrt_rel)sqrt_rel=e;
    }
    int bad=trig_err>1.0e-5f||asin_err>=0.02f||log_err>1.0e-4f||sqrt_rel>2.0e-6f;
    printf("vfpu_math: trig_abs=%g asin_abs=%g log2_abs=%g sqrt_rel=%g%s\n",
           trig_err,asin_err,log_err,sqrt_rel,bad?" FAIL":"");
    return bad;
}

static int check_unaligned_dispatch(void) {
    sr_mem_init();
    CpuState s;memset(&s,0,sizeof(s));s.vfpuCtrl[0]=s.vfpuCtrl[1]=0xe4;
    const uint32_t base=0x08001000u,pc=0x00002000u;
    s.r[4]=base;
    for(int i=0;i<4;i++)MEM_W32(base+(uint32_t)i*4,0x11111111u*(uint32_t)(i+1));
    for(int i=0;i<4;i++)s.vi[i]=0xA0A0A000u+(uint32_t)i;

    /* lvl.q vt=0, 8(r4): lanes 3,2,1 receive words 2,1,0; lane 0 survives. */
    uint32_t lvl=(0x35u<<26)|(4u<<21)|8u;
    MEM_W32(pc,lvl);s.pc=pc;dispatch(&s,SR_DISPATCH_VFPU_TAG|pc);
    int bad=s.vi[0]!=0xA0A0A000u||s.vi[1]!=0x11111111u||
            s.vi[2]!=0x22222222u||s.vi[3]!=0x33333333u||s.pc!=pc+4;

    /* lvr.q vt=0, 4(r4): lanes 0,1,2 receive words 1,2,3; lane 3 survives. */
    for(int i=0;i<4;i++)s.vi[i]=0xB0B0B000u+(uint32_t)i;
    uint32_t lvr=(0x35u<<26)|(4u<<21)|4u|2u;
    if(sr_vfpu_interp(&s,lvr)==SR_VFPU_OTHER)bad=1;
    bad|=s.vi[0]!=0x22222222u||s.vi[1]!=0x33333333u||
         s.vi[2]!=0x44444444u||s.vi[3]!=0xB0B0B003u;
    printf("vfpu_unaligned_dispatch: %s\n",bad?"FAIL":"ok");
    return bad;
}

static int check_vcrs_width_guard(void) {
    CpuState s;
    memset(&s, 0, sizeof(s));
    s.vfpuCtrl[0] = 0x12345678u;
    s.vfpuCtrl[1] = 0x23456789u;
    s.vfpuCtrl[2] = 0x3456789au;
    s.vi[0] = 0xdeadbeefu;
    const CpuState before = s;
    const uint32_t base = (0x19u << 26) | (5u << 23) | (1u << 16) | (2u << 8) | 3u;
    const uint32_t widths[] = {0u, 1u << 7, (1u << 14) | (1u << 7)};
    int bad = 0;
    for (size_t i = 0; i < sizeof(widths) / sizeof(widths[0]); i++) {
        s = before;
        if (sr_vfpu_interp(&s, base | widths[i]) != SR_VFPU_OTHER ||
            memcmp(&s, &before, sizeof(s)) != 0) {
            bad = 1;
        }
    }
    printf("vfpu_vcrs_width_guard: %s\n", bad ? "FAIL" : "ok");
    return bad;
}

int main(int argc, char **argv) {
    int trials = argc > 1 ? atoi(argv[1]) : 200;
    if (getenv("FUZZ_TRIALS")) {
        trials = atoi(getenv("FUZZ_TRIALS"));
    }

    if (argc > 2) {
        s_rng = (uint32_t)strtoul(argv[2], NULL, 0);
    }
    if (getenv("FUZZ_SEED")) {
        s_rng = (uint32_t)strtoul(getenv("FUZZ_SEED"), NULL, 0);
    }

    int tested = 0, skipped = 0;
    int bad_cases = check_transcendentals()+check_unaligned_dispatch()+check_vcrs_width_guard();
    unsigned long long mismatches = 0, total = 0;

    for (int c = 0; c < FUZZ_NCASES; c++) {
        uint32_t w = fuzz_cases[c].w;
        int case_bad = 0, interp_other = 0;
        int trials_failed = 0;
        for (int t = 0; t < trials; t++) {
            CpuState s0;
            memset(&s0, 0, sizeof(s0));
            for (int i = 0; i < 128; i++) s0.v[i] = rand_float();
            s0.vfpuCtrl[0] = rand_sprefix();
            s0.vfpuCtrl[1] = rand_sprefix();
            s0.vfpuCtrl[2] = rand_dprefix();
            s0.vfpuCtrl[3] = rng() & 0x3Fu;     /* VFPU_CC */

            /* vcrsp/vqmul and vrot have hardware-quirky prefix interactions that neither
             * side models (the game never prefixes them) — fuzz those identity-prefix only */
            uint32_t top = w >> 26, sub3 = (w >> 23) & 7;
            if (top == 0x3c && (sub3 == 5 || (sub3 == 7 && ((w >> 21) & 0x1F) == 29))) {
                s0.vfpuCtrl[0] = 0xe4; s0.vfpuCtrl[1] = 0xe4; s0.vfpuCtrl[2] = 0;
            }

            CpuState s1 = s0, s2 = s0;
            int kind = sr_vfpu_interp(&s2, w);
            if (kind == SR_VFPU_OTHER) { interp_other = 1; break; }
            /* SR_VFPU_STATE (vcmp/vpfx) still mutates vfpuCtrl — compare it like the rest */
            fuzz_run_codegen(&s1, c);
            total++;

            int bad = 0;
            for (int i = 0; i < 128; i++) {
                if (s1.vi[i] != s2.vi[i]) {
                    if (!bad && mismatches < 40)
                        fprintf(stderr,
                                "MISMATCH op=0x%08x (sample @0x%08x) trial %d: v%d codegen=0x%08x (%g) interp=0x%08x (%g) [pfx s=%05x t=%05x d=%03x cc=%02x]\n",
                                w, fuzz_cases[c].addr, t, i, s1.vi[i], s1.v[i], s2.vi[i], s2.v[i],
                                s0.vfpuCtrl[0], s0.vfpuCtrl[1], s0.vfpuCtrl[2], s0.vfpuCtrl[3]);
                    bad = 1;
                }
            }
            for (int i = 0; i < 4; i++) {
                if (s1.vfpuCtrl[i] != s2.vfpuCtrl[i]) {
                    if (!bad && mismatches < 40)
                        fprintf(stderr,
                                "MISMATCH op=0x%08x (sample @0x%08x) trial %d: vfpuCtrl[%d] codegen=0x%08x interp=0x%08x\n",
                                w, fuzz_cases[c].addr, t, i, s1.vfpuCtrl[i], s2.vfpuCtrl[i]);
                    bad = 1;
                }
            }
            if (bad) { mismatches++; case_bad = 1; trials_failed++; }
        }
        if (interp_other) {
            skipped++;
            fprintf(stderr, "UNCOVERED op=0x%08x (sample @0x%08x): interp has no oracle for it\n",
                    w, fuzz_cases[c].addr);
            continue;
        }
        tested++;
        if (case_bad) {
            fprintf(stderr,"DIVERGED op=0x%08x sample=0x%08x\n",w,fuzz_cases[c].addr);
            bad_cases++;
        }
        printf("FUZZ_PROGRESS case=%d total=%d passed=%d failed=%d op=0x%08x\n",
               c, trials, trials - trials_failed, trials_failed, w);
        fflush(stdout);
    }

    printf("vfpu_fuzz: %d/%d distinct words tested (%d not covered by interp oracle), "
           "%d words diverge, %llu/%llu trials mismatched\n",
           tested, FUZZ_NCASES, skipped, bad_cases, mismatches, total);
    return bad_cases ? 1 : 0;
}
