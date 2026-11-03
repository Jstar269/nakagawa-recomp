// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

/*
 * Sampled in-play differential checking (issue #1843), compiled only into a verify build
 * (-DSR_VERIFY, off by default). The hooks the runtime calls are declared in recomp.h;
 * this header adds the configuration a harness can set instead of the environment.
 *
 * Environment (read once, at the first call the verifier sees):
 *   SR_VERIFY=k            check the first k calls of every function, then every call whose
 *                          running count is a power of two (k may be 0)
 *   SR_VERIFY_EVERY=n      after the first k calls, check every n-th call instead
 *   SR_VERIFY_RATE=r       at most r checks per second of host time, burst r (fairness:
 *                          a function with no check yet may overdraw by up to the burst)
 *   SR_VERIFY_RANGE=lo-hi  only functions whose entry lies in [lo, hi] (hex)
 *   SR_VERIFY_STEPS=n      reference step budget per check (default 1000000)
 *   SR_VERIFY_REPORT=file  per-function report, written at exit
 *   SR_VERIFY_CAPTURE=dir  snapshots (src/rt/nksnap.h) of the first mismatches
 *   SR_VERIFY_CAPTURE_MAX=n  how many (default 4)
 * A set but malformed value stops the run with exit status 2; it never disables checking.
 */
#ifndef NK_VERIFY_H
#define NK_VERIFY_H

#include "recomp.h"

#ifdef SR_VERIFY

typedef struct {
    int enabled;              /* SR_VERIFY or SR_VERIFY_EVERY was set */
    uint32_t first_k;
    uint32_t every;           /* 0: power-of-two counts */
    double rate;              /* checks per second; 0: unlimited */
    int has_range;
    uint32_t range_lo, range_hi;
    uint64_t max_steps;
    const char *report_path;
    const char *capture_dir;
    uint32_t capture_max;
} SrVerifyConfig;

/* Fill cfg from the environment (exits with status 2 on a malformed value). */
void sr_verify_config_from_env(SrVerifyConfig *cfg);
/* Install a configuration and clear all counters. Strings must outlive the run. */
void sr_verify_configure(const SrVerifyConfig *cfg);
/* A host clock in nanoseconds, for tests. NULL restores the default. */
void sr_verify_set_clock(uint64_t (*now_ns)(void));
/* Write the report now (also done at exit when SR_VERIFY_REPORT is set). */
int sr_verify_report_write(const char *path);

#endif /* SR_VERIFY */
#endif /* NK_VERIFY_H */
