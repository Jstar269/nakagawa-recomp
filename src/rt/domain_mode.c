// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
//
// LLE Phase 1 (PR 3): per-domain mode table, NID/domain/export registries,
// and the sr_import_call() seam (spec section 4).
//
// Return contract for the seam: 0 means handled (HLE executed, COSIM recorded
// plus HLE, fallback logged plus HLE, or the LLE guest export ran); negative
// means flow_kind/flow_target carry SR_FLOW_FATAL and the caller must leave
// its native body at once, exactly like the PR 2 cpu_lle helpers. The seam
// never clears flow and never invents success.

#include "domain_mode.h"

#include "recomp.h"

#include <stdio.h>
#include <string.h>

/* ---- mode table ---------------------------------------------------------- */

static SrDomainMode s_modes[SR_DOMAIN_COUNT] = {
    SR_MODE_HLE, SR_MODE_HLE, SR_MODE_HLE,
    SR_MODE_HLE, SR_MODE_HLE, SR_MODE_HLE,
    SR_MODE_HLE, SR_MODE_HLE, SR_MODE_HLE
};
static int s_locked;

void sr_domain_lock(void) {
    s_locked = 1;
}

int sr_domain_locked(void) {
    return s_locked;
}

int sr_domain_mode_set(SrDomain domain, SrDomainMode mode) {
    if (domain < 0 || domain >= SR_DOMAIN_COUNT) {
        return -1;
    }
    if (mode < SR_MODE_HLE || mode > SR_MODE_LLE_FALLBACK_HLE) {
        return -1;
    }
    if (s_locked) {
        return -1;
    }
    s_modes[domain] = mode;
    return 0;
}

SrDomainMode sr_domain_mode_get(SrDomain domain) {
    if (domain < 0 || domain >= SR_DOMAIN_COUNT) {
        /* Fail closed to the safe default; never invent a mode. */
        return SR_MODE_HLE;
    }
    return s_modes[domain];
}

/* ---- library -> domain table (preflight mapping; exact names) ------------ */

typedef struct SrLibraryDomain {
    const char *library;
    SrDomain domain;
} SrLibraryDomain;

static const SrLibraryDomain s_library_domains[] = {
    { "ThreadManForUser", SR_DOMAIN_THREADMAN },
    { "ThreadManForKernel", SR_DOMAIN_THREADMAN },
    { "IoFileMgrForUser", SR_DOMAIN_IO },
    { "IoFileMgrForKernel", SR_DOMAIN_IO },
    { "sceGe_user", SR_DOMAIN_GE },
    { "sceAudio", SR_DOMAIN_AUDIO },
    { "sceSasCore", SR_DOMAIN_AUDIO },
    { "sceAtrac3plus", SR_DOMAIN_AUDIO },
    { "sceMpeg", SR_DOMAIN_MEDIA },
    { "sceVideocodec", SR_DOMAIN_MEDIA },
    { "InterruptManager", SR_DOMAIN_INTC },
    { "sceSysTimer", SR_DOMAIN_TIMER },
    { "SysTimerForKernel", SR_DOMAIN_TIMER }
};

/* scePsmf* is a family (scePsmf, scePsmfPlayer, ...); prefix-match it while
 * every other entry above stays an exact match. */
#define SR_PSMF_PREFIX "scePsmf"

SrDomain sr_domain_for_library(const char *library) {
    size_t i;
    if (!library || !library[0]) {
        return SR_DOMAIN_COUNT;
    }
    if (strncmp(library, SR_PSMF_PREFIX, sizeof(SR_PSMF_PREFIX) - 1u) == 0) {
        return SR_DOMAIN_MEDIA;
    }
    for (i = 0; i < sizeof(s_library_domains) / sizeof(s_library_domains[0]); i++) {
        if (strcmp(library, s_library_domains[i].library) == 0) {
            return s_library_domains[i].domain;
        }
    }
    return SR_DOMAIN_COUNT;
}

/* ---- NID -> domain bindings ---------------------------------------------- */

#define SR_DOMAIN_BIND_CAP 256u

typedef struct SrNidDomain {
    uint32_t nid;
    SrDomain domain;
} SrNidDomain;

static SrNidDomain s_nid_domains[SR_DOMAIN_BIND_CAP];
static unsigned s_nid_domain_n;

int sr_domain_bind_nid(SrDomain domain, uint32_t nid) {
    unsigned i;
    if (domain < 0 || domain >= SR_DOMAIN_COUNT) {
        return -1;
    }
    if (nid == 0u) {
        return -1;
    }
    if (s_locked) {
        return -1;
    }
    for (i = 0; i < s_nid_domain_n; i++) {
        if (s_nid_domains[i].nid == nid) {
            if (s_nid_domains[i].domain == domain) {
                return 0;
            }
            /* A conflicting rebind keeps the original (fail closed). */
            return -1;
        }
    }
    if (s_nid_domain_n >= SR_DOMAIN_BIND_CAP) {
        return -1;
    }
    s_nid_domains[s_nid_domain_n].nid = nid;
    s_nid_domains[s_nid_domain_n].domain = domain;
    s_nid_domain_n++;
    return 0;
}

SrDomain sr_domain_lookup_nid(uint32_t nid) {
    unsigned i;
    for (i = 0; i < s_nid_domain_n; i++) {
        if (s_nid_domains[i].nid == nid) {
            return s_nid_domains[i].domain;
        }
    }
    return SR_DOMAIN_COUNT;
}

/* ---- guest-export registry ------------------------------------------------ */

#define SR_IMPORT_EXPORT_CAP 256u

typedef struct SrImportExport {
    uint32_t nid;
    uint32_t guest_addr;
} SrImportExport;

static SrImportExport s_exports[SR_IMPORT_EXPORT_CAP];
static unsigned s_export_n;

int sr_import_register_export(uint32_t nid, uint32_t guest_addr) {
    unsigned i;
    if (nid == 0u || guest_addr == 0u || (guest_addr & 3u) != 0u) {
        return -1;
    }
    if (s_locked) {
        return -1;
    }
    for (i = 0; i < s_export_n; i++) {
        if (s_exports[i].nid == nid) {
            if (s_exports[i].guest_addr == guest_addr) {
                return 0;
            }
            /* A conflicting address keeps the original (fail closed). */
            fprintf(stderr,
                    "SR_IMPORT_FATAL: nid 0x%08x already exports 0x%08x; "
                    "refusing 0x%08x\n",
                    nid, s_exports[i].guest_addr, guest_addr);
            return -1;
        }
    }
    if (s_export_n >= SR_IMPORT_EXPORT_CAP) {
        return -1;
    }
    s_exports[s_export_n].nid = nid;
    s_exports[s_export_n].guest_addr = guest_addr;
    s_export_n++;
    return 0;
}

int sr_import_lookup_export(uint32_t nid, uint32_t *guest_addr_out) {
    unsigned i;
    for (i = 0; i < s_export_n; i++) {
        if (s_exports[i].nid == nid) {
            if (guest_addr_out) {
                *guest_addr_out = s_exports[i].guest_addr;
            }
            return 0;
        }
    }
    return -1;
}

/* ---- transition accounting ------------------------------------------------ */

static uint32_t s_fallback_count;
static uint32_t s_cosim_count;
static uint32_t s_last_fallback_nid;
static uint32_t s_last_cosim_nid;

uint32_t sr_domain_fallback_count(void) {
    return s_fallback_count;
}

uint32_t sr_domain_cosim_request_count(void) {
    return s_cosim_count;
}

uint32_t sr_domain_last_fallback_nid(void) {
    return s_last_fallback_nid;
}

uint32_t sr_domain_last_cosim_nid(void) {
    return s_last_cosim_nid;
}

void sr_domain_reset_defaults(void) {
    int d;
    for (d = 0; d < (int)SR_DOMAIN_COUNT; d++) {
        s_modes[d] = SR_MODE_HLE;
    }
    s_nid_domain_n = 0u;
    s_export_n = 0u;
    s_fallback_count = 0u;
    s_cosim_count = 0u;
    s_last_fallback_nid = 0u;
    s_last_cosim_nid = 0u;
    s_locked = 0;
}

/* ---- the seam ------------------------------------------------------------- */

static int sr_import_fatal(
    CpuState *s,
    uint32_t nid,
    uint32_t stub_pc,
    const char *reason) {
    fprintf(stderr,
            "SR_IMPORT_FATAL: nid 0x%08x stub_pc 0x%08x: %s\n",
            nid, stub_pc, reason);
    if (s) {
        s->flow_kind = SR_FLOW_FATAL;
        s->flow_target = nid;
    }
    return -1;
}

/* Run a registered guest export through the production linked-call boundary
 * (explicit target plus resume PC; live $ra is not a resume descriptor).
 * A dispatch rejection fails closed like a missing export: falling through
 * to HLE after a partially executed guest body could double-apply effects. */
static int sr_import_call_guest(CpuState *s, uint32_t nid, uint32_t stub_pc) {
    uint32_t guest_addr;
    int rc;
    if (sr_import_lookup_export(nid, &guest_addr) != 0) {
        return sr_import_fatal(s, nid, stub_pc, "no registered guest export");
    }
    rc = dispatch_call_try(s, guest_addr, stub_pc);
    if (rc < 0) {
        return sr_import_fatal(s, nid, stub_pc, "guest export dispatch rejected");
    }
    return 0;
}

int sr_import_call(CpuState *s, uint32_t nid, uint32_t stub_pc) {
    SrDomain domain;
    SrDomainMode mode;
    if (!s) {
        fprintf(stderr,
                "SR_IMPORT_FATAL: nid 0x%08x stub_pc 0x%08x: null CpuState\n",
                nid, stub_pc);
        return -1;
    }
    domain = sr_domain_lookup_nid(nid);
    mode = domain < SR_DOMAIN_COUNT ? s_modes[domain] : SR_MODE_HLE;
    switch (mode) {
    case SR_MODE_HLE:
        /* Exactly the existing HLE handler path: same callee, same args, no
         * wrapper logic that could alter effects. Unknown-NID fatal policy
         * inside sr_syscall() is unchanged. */
        return (int)sr_syscall(s, nid);
    case SR_MODE_COSIM:
        /* PR 3 records the comparison request and runs the HLE lane. The
         * actual dual execution and difference report are PR 8; there is no
         * implicit fallback here, only the configured HLE lane. */
        s_cosim_count++;
        s_last_cosim_nid = nid;
        return (int)sr_syscall(s, nid);
    case SR_MODE_LLE:
        if (domain >= SR_DOMAIN_COUNT) {
            /* Unmapped libraries are HLE-only and never take the LLE guest
             * path; strict LLE on one fails closed instead of silently
             * running HLE. */
            return sr_import_fatal(s, nid, stub_pc, "NID has no domain binding");
        }
        return sr_import_call_guest(s, nid, stub_pc);
    case SR_MODE_LLE_FALLBACK_HLE:
        if (domain < SR_DOMAIN_COUNT &&
            sr_import_lookup_export(nid, NULL) == 0) {
            return sr_import_call_guest(s, nid, stub_pc);
        }
        s_fallback_count++;
        s_last_fallback_nid = nid;
        fprintf(stderr,
                "SR_IMPORT_FALLBACK_HLE: nid 0x%08x stub_pc 0x%08x: "
                "no guest export; using explicit HLE fallback\n",
                nid, stub_pc);
        return (int)sr_syscall(s, nid);
    default:
        break;
    }
    /* Unreachable through the validated setter; memory corruption must still
     * fail closed rather than pick a lane. */
    return sr_import_fatal(s, nid, stub_pc, "invalid domain mode");
}
