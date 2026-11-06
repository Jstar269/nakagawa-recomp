// SPDX-License-Identifier: GPL-3.0-or-later
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
#include "flight_recorder.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sr_cosim_reset(void);

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
                    "SR_IMPORT_FATAL: nid 0x%08" PRIx32 " already exports 0x%08" PRIx32 "; "
                    "refusing 0x%08" PRIx32 "\n",
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
    sr_cosim_reset();
    s_locked = 0;
}

/* ---- the seam ------------------------------------------------------------- */

static int sr_import_fatal(
    CpuState *s,
    uint32_t nid,
    uint32_t stub_pc,
    const char *reason) {
    sr_flight_fatal(SR_FLIGHT_KIND_FATAL_IMPORT, stub_pc, nid, 0u);
    fprintf(stderr,
            "SR_IMPORT_FATAL: nid 0x%08" PRIx32 " stub_pc 0x%08" PRIx32 ": %s\n",
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
 * to HLE after a partially executed guest body could double-apply effects.
 * The COSIM guest lane (cosim_lane != 0) takes the same entry but reports a
 * missing export or a rejection to its caller instead: the caller restores
 * the input state and records the call as NOT_COMPARED, and the run goes on
 * with the HLE lane. */
static int sr_import_call_guest(CpuState *s, uint32_t nid, uint32_t stub_pc, int cosim_lane) {
    uint32_t guest_addr;
    int rc;
    if (sr_import_lookup_export(nid, &guest_addr) != 0) {
        return cosim_lane ? -1 : sr_import_fatal(s, nid, stub_pc, "no registered guest export");
    }
    rc = dispatch_call_try(s, guest_addr, stub_pc);
    if (rc < 0) {
        return cosim_lane ? rc : sr_import_fatal(s, nid, stub_pc, "guest export dispatch rejected");
    }
    return 0;
}

/* ---- COSIM dual run (PR 8, #2148) ------------------------------------------ */

#define SR_COSIM_PAGE 4096u
#define SR_COSIM_DEAD_STACK_DEFAULT 0x10000u
#define SR_COSIM_MATCH_LOG_CAP 256u

/* Guest physical windows the comparison covers, ascending. The arena maps
 * physical [0, 0x0c000000) with RAM at g_mem; the eDRAM CPU mirrors alias the
 * first 2 MiB of 0x04000000, so that window covers them. */
typedef struct SrCosimWindow {
    uint32_t base;
    uint32_t size;
} SrCosimWindow;

static const SrCosimWindow s_cosim_windows[] = {
    { 0x00010000u, 0x00004000u },  /* scratchpad */
    { 0x04000000u, 0x00200000u },  /* eDRAM */
    { 0x08000000u, 0x04000000u },  /* RAM */
};
#define SR_COSIM_WINDOW_COUNT (sizeof(s_cosim_windows) / sizeof(s_cosim_windows[0]))

/* Callee-saved GPRs compared after both lanes: s0-s7, gp, sp, fp. */
static const uint8_t s_cosim_saved_regs[] = { 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30 };

static uint8_t *s_cosim_snapshot;     /* the windows back to back */
static uint32_t *s_cosim_dirty_addr;  /* guest-lane pages, ascending */
static uint8_t *s_cosim_dirty_data;
static size_t s_cosim_dirty_n;
static size_t s_cosim_dirty_cap;
static int s_cosim_active;            /* a cosim call is between its lanes */
static int s_cosim_guest_active;      /* the guest lane is running */
static uint32_t s_cosim_nested_nid;
static uint32_t s_cosim_dead_stack = SR_COSIM_DEAD_STACK_DEFAULT;
static uint32_t s_cosim_match_n;
static uint32_t s_cosim_diff_n;
static uint32_t s_cosim_not_compared_n;
static SrCosimResult s_cosim_last;
static uint32_t s_cosim_logged_match[SR_COSIM_MATCH_LOG_CAP];
static unsigned s_cosim_logged_match_n;

static void sr_cosim_reset(void) {
    s_cosim_dirty_n = 0u;
    s_cosim_active = 0;
    s_cosim_guest_active = 0;
    s_cosim_nested_nid = 0u;
    s_cosim_dead_stack = SR_COSIM_DEAD_STACK_DEFAULT;
    s_cosim_match_n = 0u;
    s_cosim_diff_n = 0u;
    s_cosim_not_compared_n = 0u;
    memset(&s_cosim_last, 0, sizeof s_cosim_last);
    s_cosim_logged_match_n = 0u;
}

uint32_t sr_domain_cosim_match_count(void) {
    return s_cosim_match_n;
}

uint32_t sr_domain_cosim_diff_count(void) {
    return s_cosim_diff_n;
}

uint32_t sr_domain_cosim_not_compared_count(void) {
    return s_cosim_not_compared_n;
}

void sr_domain_cosim_last_result(SrCosimResult *out) {
    if (out) {
        *out = s_cosim_last;
    }
}

void sr_domain_cosim_set_dead_stack(uint32_t bytes) {
    s_cosim_dead_stack = bytes;
}

void sr_domain_cosim_release(void) {
    free(s_cosim_snapshot);
    free(s_cosim_dirty_addr);
    free(s_cosim_dirty_data);
    s_cosim_snapshot = NULL;
    s_cosim_dirty_addr = NULL;
    s_cosim_dirty_data = NULL;
    s_cosim_dirty_n = 0u;
    s_cosim_dirty_cap = 0u;
}

static uint8_t *sr_cosim_host(uint32_t phys) {
    return g_mem + (int32_t)(phys - SR_RAM_BASE);
}

static size_t sr_cosim_total_bytes(void) {
    size_t total = 0u;
    size_t w;
    for (w = 0; w < SR_COSIM_WINDOW_COUNT; w++) {
        total += s_cosim_windows[w].size;
    }
    return total;
}

static const char *sr_cosim_reason_text(SrCosimReason reason) {
    switch (reason) {
    case SR_COSIM_REASON_UNPAIRED: return "no registered guest export";
    case SR_COSIM_REASON_NESTED_IMPORT: return "the guest lane reached an import";
    case SR_COSIM_REASON_GUEST_REJECTED: return "dispatch rejected the guest export";
    case SR_COSIM_REASON_REENTRY: return "cosim call inside another cosim call";
    case SR_COSIM_REASON_NO_ARENA: return "guest memory is not initialised";
    case SR_COSIM_REASON_NO_MEMORY: return "no memory for the snapshot";
    default: return "unknown";
    }
}

static void sr_cosim_begin_result(const CpuState *s, uint32_t nid, uint32_t stub_pc) {
    memset(&s_cosim_last, 0, sizeof s_cosim_last);
    s_cosim_last.nid = nid;
    s_cosim_last.stub_pc = stub_pc;
    s_cosim_last.args[0] = s->r[4];
    s_cosim_last.args[1] = s->r[5];
    s_cosim_last.args[2] = s->r[6];
    s_cosim_last.args[3] = s->r[7];
}

/* Record a NOT_COMPARED call. The HLE lane still runs (by the caller). */
static void sr_cosim_not_compared(SrCosimReason reason) {
    s_cosim_last.outcome = SR_COSIM_OUTCOME_NOT_COMPARED;
    s_cosim_last.reason = reason;
    s_cosim_not_compared_n++;
    if (reason == SR_COSIM_REASON_NESTED_IMPORT) {
        fprintf(stderr,
                "SR_COSIM_NOT_COMPARED: nid 0x%08" PRIx32 " stub_pc 0x%08" PRIx32 ": %s "
                "(nid 0x%08" PRIx32 ")\n",
                s_cosim_last.nid, s_cosim_last.stub_pc, sr_cosim_reason_text(reason),
                s_cosim_last.nested_nid);
    } else {
        fprintf(stderr,
                "SR_COSIM_NOT_COMPARED: nid 0x%08" PRIx32 " stub_pc 0x%08" PRIx32 ": %s\n",
                s_cosim_last.nid, s_cosim_last.stub_pc, sr_cosim_reason_text(reason));
    }
}

static int sr_cosim_snapshot_take(void) {
    size_t off = 0u;
    size_t w;
    if (!s_cosim_snapshot) {
        s_cosim_snapshot = (uint8_t *)malloc(sr_cosim_total_bytes());
        if (!s_cosim_snapshot) {
            return -1;
        }
    }
    for (w = 0; w < SR_COSIM_WINDOW_COUNT; w++) {
        memcpy(s_cosim_snapshot + off, sr_cosim_host(s_cosim_windows[w].base),
               s_cosim_windows[w].size);
        off += s_cosim_windows[w].size;
    }
    return 0;
}

static int sr_cosim_dirty_push(uint32_t phys, const uint8_t *page) {
    if (s_cosim_dirty_n == s_cosim_dirty_cap) {
        size_t cap = s_cosim_dirty_cap ? s_cosim_dirty_cap * 2u : 64u;
        uint32_t *addr = (uint32_t *)realloc(s_cosim_dirty_addr, cap * sizeof *addr);
        if (!addr) {
            return -1;
        }
        s_cosim_dirty_addr = addr;
        uint8_t *data = (uint8_t *)realloc(s_cosim_dirty_data, cap * SR_COSIM_PAGE);
        if (!data) {
            return -1;
        }
        s_cosim_dirty_data = data;
        s_cosim_dirty_cap = cap;
    }
    s_cosim_dirty_addr[s_cosim_dirty_n] = phys;
    memcpy(s_cosim_dirty_data + s_cosim_dirty_n * SR_COSIM_PAGE, page, SR_COSIM_PAGE);
    s_cosim_dirty_n++;
    return 0;
}

/* Keep every page the guest lane changed, then put the input state back.
 * Restoring always completes, even when keeping a page fails: the HLE lane
 * must start from the input state whatever the comparison can still say. */
static int sr_cosim_capture_guest_and_restore(void) {
    size_t off = 0u;
    size_t w;
    int kept = 0;
    s_cosim_dirty_n = 0u;
    for (w = 0; w < SR_COSIM_WINDOW_COUNT; w++) {
        uint32_t page;
        for (page = 0; page < s_cosim_windows[w].size; page += SR_COSIM_PAGE) {
            uint8_t *live = sr_cosim_host(s_cosim_windows[w].base + page);
            const uint8_t *before = s_cosim_snapshot + off + page;
            if (memcmp(live, before, SR_COSIM_PAGE) != 0) {
                if (kept == 0 &&
                    sr_cosim_dirty_push(s_cosim_windows[w].base + page, live) != 0) {
                    kept = -1;
                }
                memcpy(live, before, SR_COSIM_PAGE);
            }
        }
        off += s_cosim_windows[w].size;
    }
    return kept;
}

static int sr_cosim_in_dead_stack(uint32_t phys, uint32_t sp_phys) {
    return s_cosim_dead_stack != 0u && phys < sp_phys && sp_phys - phys <= s_cosim_dead_stack;
}

/* Compare the guest lane's memory (snapshot plus kept pages) with live memory,
 * which now holds the HLE lane's result. */
static void sr_cosim_compare_memory(uint32_t sp_phys) {
    size_t off = 0u;
    size_t dirty = 0u;
    size_t w;
    int first = s_cosim_last.first_kind == SR_COSIM_DIFF_NONE;
    for (w = 0; w < SR_COSIM_WINDOW_COUNT; w++) {
        uint32_t page;
        for (page = 0; page < s_cosim_windows[w].size; page += SR_COSIM_PAGE) {
            uint32_t phys = s_cosim_windows[w].base + page;
            const uint8_t *guest = s_cosim_snapshot + off + page;
            const uint8_t *hle = sr_cosim_host(phys);
            uint32_t i;
            if (dirty < s_cosim_dirty_n && s_cosim_dirty_addr[dirty] == phys) {
                guest = s_cosim_dirty_data + dirty * SR_COSIM_PAGE;
                dirty++;
            }
            if (memcmp(guest, hle, SR_COSIM_PAGE) == 0) {
                continue;
            }
            for (i = 0; i < SR_COSIM_PAGE; i++) {
                if (guest[i] == hle[i] || sr_cosim_in_dead_stack(phys + i, sp_phys)) {
                    continue;
                }
                s_cosim_last.diff_bytes++;
                if (first) {
                    first = 0;
                    s_cosim_last.first_kind = SR_COSIM_DIFF_MEMORY;
                    s_cosim_last.first_addr = phys + i;
                    s_cosim_last.guest_byte = guest[i];
                    s_cosim_last.hle_byte = hle[i];
                }
            }
        }
        off += s_cosim_windows[w].size;
    }
}

static void sr_cosim_report(void) {
    const SrCosimResult *r = &s_cosim_last;
    unsigned i;
    if (r->first_kind == SR_COSIM_DIFF_NONE && r->diff_bytes == 0u) {
        s_cosim_last.outcome = SR_COSIM_OUTCOME_MATCH;
        s_cosim_match_n++;
        for (i = 0; i < s_cosim_logged_match_n; i++) {
            if (s_cosim_logged_match[i] == r->nid) {
                return;
            }
        }
        if (s_cosim_logged_match_n < SR_COSIM_MATCH_LOG_CAP) {
            s_cosim_logged_match[s_cosim_logged_match_n++] = r->nid;
        }
        fprintf(stderr, "SR_COSIM_MATCH: nid 0x%08" PRIx32 " (first match; later ones counted)\n",
                r->nid);
        return;
    }
    s_cosim_last.outcome = SR_COSIM_OUTCOME_DIFF;
    s_cosim_diff_n++;
    fprintf(stderr,
            "SR_COSIM_DIFF: nid 0x%08" PRIx32 " stub_pc 0x%08" PRIx32 " args 0x%08" PRIx32
            " 0x%08" PRIx32 " 0x%08" PRIx32 " 0x%08" PRIx32 " v0 guest 0x%08" PRIx32
            " hle 0x%08" PRIx32 "; ",
            r->nid, r->stub_pc, r->args[0], r->args[1], r->args[2], r->args[3],
            r->guest_v0, r->hle_v0);
    switch (r->first_kind) {
    case SR_COSIM_DIFF_V0:
        fprintf(stderr, "first difference v0");
        break;
    case SR_COSIM_DIFF_REGISTER:
        fprintf(stderr, "first difference r%" PRIu32 " guest 0x%08" PRIx32 " hle 0x%08" PRIx32,
                r->diff_reg, r->guest_reg, r->hle_reg);
        break;
    default:
        fprintf(stderr, "first difference byte 0x%08" PRIx32 " guest 0x%02x hle 0x%02x",
                r->first_addr, (unsigned)r->guest_byte, (unsigned)r->hle_byte);
        break;
    }
    fprintf(stderr, "; %" PRIu32 " memory bytes differ\n", r->diff_bytes);
}

/* The COSIM arm. Returns the HLE lane's handler value, as SR_MODE_HLE does. */
static int sr_import_call_cosim(CpuState *s, uint32_t nid, uint32_t stub_pc) {
    CpuState input;
    CpuState guest;
    uint32_t ret;
    int rc;
    unsigned i;
    if (s_cosim_active) {
        /* A cosim call inside the HLE lane of another (a guest callback the
         * handler runs) cannot take a second snapshot of the same memory. It
         * is counted and logged, and leaves the outer call's result alone. */
        s_cosim_not_compared_n++;
        fprintf(stderr,
                "SR_COSIM_NOT_COMPARED: nid 0x%08" PRIx32 " stub_pc 0x%08" PRIx32 ": %s\n",
                nid, stub_pc, sr_cosim_reason_text(SR_COSIM_REASON_REENTRY));
        return (int)sr_syscall(s, nid);
    }
    sr_cosim_begin_result(s, nid, stub_pc);
    if (sr_import_lookup_export(nid, NULL) != 0) {
        sr_cosim_not_compared(SR_COSIM_REASON_UNPAIRED);
        return (int)sr_syscall(s, nid);
    }
    if (!g_mem) {
        sr_cosim_not_compared(SR_COSIM_REASON_NO_ARENA);
        return (int)sr_syscall(s, nid);
    }
    if (sr_cosim_snapshot_take() != 0) {
        sr_cosim_not_compared(SR_COSIM_REASON_NO_MEMORY);
        return (int)sr_syscall(s, nid);
    }

    s_cosim_active = 1;
    input = *s;
    s_cosim_nested_nid = 0u;
    s_cosim_guest_active = 1;
    rc = sr_import_call_guest(s, nid, stub_pc, 1);
    s_cosim_guest_active = 0;
    guest = *s;
    int kept = sr_cosim_capture_guest_and_restore();
    *s = input;

    ret = sr_syscall(s, nid);
    s_cosim_active = 0;

    if (rc < 0 || guest.flow_kind != SR_FLOW_NONE) {
        if (s_cosim_nested_nid != 0u) {
            s_cosim_last.nested_nid = s_cosim_nested_nid;
            sr_cosim_not_compared(SR_COSIM_REASON_NESTED_IMPORT);
        } else {
            sr_cosim_not_compared(SR_COSIM_REASON_GUEST_REJECTED);
        }
        return (int)ret;
    }
    if (kept != 0) {
        sr_cosim_not_compared(SR_COSIM_REASON_NO_MEMORY);
        return (int)ret;
    }

    s_cosim_last.guest_v0 = guest.r[2];
    s_cosim_last.hle_v0 = ret;
    if (guest.r[2] != ret) {
        s_cosim_last.first_kind = SR_COSIM_DIFF_V0;
    }
    for (i = 0; i < sizeof s_cosim_saved_regs; i++) {
        unsigned reg = s_cosim_saved_regs[i];
        if (s_cosim_last.first_kind == SR_COSIM_DIFF_NONE && guest.r[reg] != s->r[reg]) {
            s_cosim_last.first_kind = SR_COSIM_DIFF_REGISTER;
            s_cosim_last.diff_reg = reg;
            s_cosim_last.guest_reg = guest.r[reg];
            s_cosim_last.hle_reg = s->r[reg];
        }
    }
    sr_cosim_compare_memory(SR_PHYS(input.r[29]));
    sr_cosim_report();
    return (int)ret;
}

int sr_import_call(CpuState *s, uint32_t nid, uint32_t stub_pc) {
    SrDomain domain;
    SrDomainMode mode;
    if (!s) {
        fprintf(stderr,
                "SR_IMPORT_FATAL: nid 0x%08" PRIx32 " stub_pc 0x%08" PRIx32 ": null CpuState\n",
                nid, stub_pc);
        return -1;
    }
    if (s_cosim_guest_active) {
        /* The guest lane of a cosim call reached an import. Running it would
         * apply a host-side effect the HLE lane applies again, so the lane
         * unwinds here and the call is reported NOT_COMPARED. */
        if (s_cosim_nested_nid == 0u) {
            s_cosim_nested_nid = nid;
        }
        s->flow_kind = SR_FLOW_FATAL;
        s->flow_target = nid;
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
        /* Both lanes run from the same input; the HLE lane's result is kept
         * and the guest lane's is compared with it (PR 8, #2148). */
        s_cosim_count++;
        s_last_cosim_nid = nid;
        return sr_import_call_cosim(s, nid, stub_pc);
    case SR_MODE_LLE:
        if (domain >= SR_DOMAIN_COUNT) {
            /* Unmapped libraries are HLE-only and never take the LLE guest
             * path; strict LLE on one fails closed instead of silently
             * running HLE. */
            return sr_import_fatal(s, nid, stub_pc, "NID has no domain binding");
        }
        return sr_import_call_guest(s, nid, stub_pc, 0);
    case SR_MODE_LLE_FALLBACK_HLE:
        if (domain < SR_DOMAIN_COUNT &&
            sr_import_lookup_export(nid, NULL) == 0) {
            return sr_import_call_guest(s, nid, stub_pc, 0);
        }
        s_fallback_count++;
        s_last_fallback_nid = nid;
        fprintf(stderr,
                "SR_IMPORT_FALLBACK_HLE: nid 0x%08" PRIx32 " stub_pc 0x%08" PRIx32 ": "
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
