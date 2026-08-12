// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/* Nakagawa-side VFPU transcendental oracle (Loop A of docs/HARDWARE_ORACLE.md).
 *
 * Drives the production sr_vfpu_* implementations in src/rt/recomp.c over the
 * same shared input vector the PSP probe uses, and emits the identical record
 * shape.  Link it exactly like the vfpu_fuzz target does -- sr_vfpu_* and the
 * table loader live in recomp.c, and the tables come from assets/vfpu/ (override
 * the directory with PSP_VFPU_TABLES).
 *
 * This is deliberately NOT a reimplementation: every value must come from the
 * same code path the game uses, so that a divergence against hardware is a real
 * finding about assets/vfpu/ rather than about this harness.
 */

#define _CRT_SECURE_NO_WARNINGS
#include "recomp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vfpu_oracle_cases.h"

static uint32_t f2b(float f) { uint32_t b; memcpy(&b, &f, 4); return b; }
static float    b2f(uint32_t b) { float f; memcpy(&f, &b, 4); return f; }

typedef float (*host_fn)(float);

typedef struct {
    const char *case_id;
    host_fn     fn;
} HostCase;

static const HostCase CASES[] = {
    { "vfpu-vrcp",  sr_vfpu_rcp   },
    { "vfpu-vrsq",  sr_vfpu_rsqrt },
    { "vfpu-vsqrt", sr_vfpu_sqrt  },
    { "vfpu-vasin", sr_vfpu_asin  },
    { "vfpu-vlog2", sr_vfpu_log2  },
    { "vfpu-vsin",  sr_vfpu_sin   },
    { "vfpu-vcos",  sr_vfpu_cos   },
    { "vfpu-vexp2", sr_vfpu_exp2  },
};

static const char *arg_after(int argc, char **argv, const char *name) {
    for (int i = 1; i + 1 < argc; ++i)
        if (strcmp(argv[i], name) == 0) return argv[i + 1];
    return NULL;
}

int main(int argc, char **argv) {
    const char *model    = arg_after(argc, argv, "--model");
    const char *firmware = arg_after(argc, argv, "--firmware");
    const char *commit   = arg_after(argc, argv, "--source-commit");
    const char *sha      = arg_after(argc, argv, "--artifact-sha256");
    if (!model || !firmware || !commit || !sha) {
        fprintf(stderr,
                "usage: vfpu_oracle_host --model M --firmware F "
                "--source-commit GIT_OID --artifact-sha256 HEX64\n"
                "  (all four are required; unmeasured provenance must fail the "
                "acceptance gate rather than default to a placeholder)\n");
        return 2;
    }

    printf("NAKAGAWA_PSP_META schema=1 source=nakagawa model=%s firmware=%s "
           "binary_sha256=%s source_commit=%s fixture=nakagawa-vfpu-oracle-v1\n",
           model, firmware, sha, commit);

    for (unsigned c = 0; c < sizeof(CASES) / sizeof(CASES[0]); ++c) {
        uint32_t digest = vfpu_oracle_digest_init();
        uint32_t spot[3] = { 0u, 0u, 0u };
        for (unsigned i = 0; i < VFPU_ORACLE_INPUT_COUNT; ++i) {
            const uint32_t r = f2b(CASES[c].fn(b2f(VFPU_ORACLE_INPUTS[i])));
            digest = vfpu_oracle_digest_step(digest, r);
            if (i == 2u)  spot[0] = r;
            if (i == 9u)  spot[1] = r;
            if (i == 12u) spot[2] = r;
        }
        printf("NAKAGAWA_PSP_TEST schema=1 test_id=PSP-VFPU-001 case_id=%s "
               "status=PASS result=0x%08x out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x\n",
               CASES[c].case_id, digest, spot[0], spot[1], spot[2],
               (unsigned int)VFPU_ORACLE_INPUT_COUNT);
    }
    return 0;
}
