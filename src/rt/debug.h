// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
// Centralized debug framework for the PSP recompiler runtime.
// All debug output is controlled via environment variables, grouped into
// bitmask categories for easy toggling. Individual env vars still work
// for backward compatibility; SR_DEBUG provides a single toggle point.
//
// Usage:
//   SR_DEBUG=0xFF  hst.exe ...          — enable all debug categories
//   SR_DEBUG=0x03  hst.exe ...          — enable MEM + HLE only
//   SR_HLELOG=1    hst.exe ...          — legacy: enable HLE logging
//
// Category bits (OR together):
//   Bit 0 (0x01) SR_DBG_MEM    — memory access logging (OOR, MEM_TRAP)
//   Bit 1 (0x02) SR_DBG_HLE    — HLE syscall dispatch tracing
//   Bit 2 (0x04) SR_DBG_SCHED  — thread scheduling events
//   Bit 3 (0x08) SR_DBG_GE     — GE command processing
//   Bit 4 (0x10) SR_DBG_INPUT  — input state changes
//   Bit 5 (0x20) SR_DBG_FS     — filesystem / I/O operations
//   Bit 6 (0x40) SR_DBG_VIDEO  — display, framebuffer, vblank
//   Bit 7 (0x80) SR_DBG_MISC   — everything else (fonts, callbacks, etc.)

#ifndef PSP_RECOMP_RT_DEBUG_H
#define PSP_RECOMP_RT_DEBUG_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

// ---- debug category bits ----
#define SR_DBG_MEM    0x01u
#define SR_DBG_HLE    0x02u
#define SR_DBG_SCHED  0x04u
#define SR_DBG_GE     0x08u
#define SR_DBG_INPUT  0x10u
#define SR_DBG_FS     0x20u
#define SR_DBG_VIDEO  0x40u
#define SR_DBG_MISC   0x80u
#define SR_DBG_ALL    0xFFu

// ---- global debug mask (set once at startup from SR_DEBUG env var) ----
extern uint32_t g_sr_debug;

// ---- fast check macro: SR_DBG(bit) is false 99.9% of the time ----
// Uses __builtin_expect for branch prediction; the compiler will inline
// the mask check and elide the fprintf call when the bit is clear.
#define SR_DBG(bit) __builtin_expect(!!(g_sr_debug & (bit)), 0)

// ---- legacy env-var compatibility ----
// Each legacy var, when set to "1", enables its corresponding category.
// SR_DEBUG takes precedence when both are set.
static inline uint32_t sr_debug_init(void) {
    uint32_t mask = 0;
    const char *e = getenv("SR_DEBUG");
    if (e) {
        mask = (uint32_t)strtoul(e, NULL, 0);
    } else {
        // Legacy individual env vars (backward compatible)
        if (getenv("SR_OORLOG")   || getenv("SR_BREAKLOG"))  mask |= SR_DBG_MEM;
        if (getenv("SR_HLELOG")   || getenv("SR_NIDLOG"))    mask |= SR_DBG_HLE;
        if (getenv("SR_THLOG")    || getenv("SR_BLOCKLOG"))  mask |= SR_DBG_SCHED;
        if (getenv("SR_GELOG")    || getenv("SR_GEWATCH"))   mask |= SR_DBG_GE;
        if (getenv("SR_INLOG")    || getenv("SR_PAD"))       mask |= SR_DBG_INPUT;
        if (getenv("SR_IOLOG")    || getenv("SR_STATLOG"))   mask |= SR_DBG_FS;
        if (getenv("SR_VBLOG")    || getenv("SR_FBSNAP"))    mask |= SR_DBG_VIDEO;
        if (getenv("SR_FONTLOG")  || getenv("SR_MPEGLOG") ||
            getenv("SR_DLGLOG")   || getenv("SR_CBLOG") ||
            getenv("SR_SYSLOG")   || getenv("SR_WAKELOG"))   mask |= SR_DBG_MISC;
    }
    return mask;
}

// ---- debug output helpers ----
// These are inline to avoid function-call overhead when disabled.

static inline void dbg_mem(uint32_t addr, uint32_t val, int write, uint32_t pc) {
    if (SR_DBG(SR_DBG_MEM)) {
        fprintf(stderr, "MEM_%s: addr=0x%08x val=0x%08x pc=0x%08x\n",
                write ? "W" : "R", addr, val, pc);
    }
}

static inline void dbg_hle(uint32_t nid, const char *name, uint32_t pc) {
    if (SR_DBG(SR_DBG_HLE)) {
        fprintf(stderr, "HLE: nid=0x%08x(%s) pc=0x%08x\n", nid, name ? name : "?", pc);
    }
}

static inline void dbg_sched(const char *event, uint32_t uid, uint32_t detail) {
    if (SR_DBG(SR_DBG_SCHED)) {
        fprintf(stderr, "SCHED: %s uid=0x%x detail=0x%x\n", event, uid, detail);
    }
}

static inline void dbg_ge(const char *event, uint32_t param) {
    if (SR_DBG(SR_DBG_GE)) {
        fprintf(stderr, "GE: %s param=0x%08x\n", event, param);
    }
}

static inline void dbg_input(uint32_t buttons) {
    if (SR_DBG(SR_DBG_INPUT)) {
        fprintf(stderr, "INPUT: buttons=0x%08x\n", buttons);
    }
}

static inline void dbg_fs(const char *op, const char *path, uint32_t result) {
    if (SR_DBG(SR_DBG_FS)) {
        fprintf(stderr, "FS: %s(%s) -> 0x%08x\n", op, path ? path : "?", result);
    }
}

static inline void dbg_video(const char *event, uint32_t param) {
    if (SR_DBG(SR_DBG_VIDEO)) {
        fprintf(stderr, "VIDEO: %s param=0x%08x\n", event, param);
    }
}

static inline void dbg_misc(const char *fmt, ...) {
    if (SR_DBG(SR_DBG_MISC)) {
        va_list ap;
        va_start(ap, fmt);
        fprintf(stderr, "MISC: ");
        vfprintf(stderr, fmt, ap);
        fprintf(stderr, "\n");
        va_end(ap);
    }
}

// ---- memory trap replacement ----
// Replaces the old hardcoded MEM_TRAP in recomp.h. Use SR_DBG_MEM_TRAP()
// in memory write paths to conditionally log writes to watched addresses.
// Call sr_add_mem_watch(addr, addr_end) at startup to set watch ranges.

#define SR_MAX_MEM_WATCHES 16
typedef struct {
    uint32_t start;
    uint32_t end;
    uint32_t value;
    const char *label;
    int match_value;
} SrMemWatch;

extern SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
extern int g_sr_mem_watch_count;
extern int g_sr_metadata_watch;
extern uint32_t g_sr_mem_watch_context_pc;
extern unsigned g_sr_mem_watch_context_limit;
extern unsigned g_sr_mem_watch_context_count;
extern int g_sr_mem_watch_context_fpr;
extern uint32_t g_sr_mem_watch_context_fpr_value;
extern uint32_t g_sr_store_context_pc;
extern unsigned g_sr_store_context_limit;
extern unsigned g_sr_store_context_count;
extern int g_sr_store_context_mem_gpr;
extern uint32_t g_sr_store_context_mem_offset;
extern unsigned g_sr_store_context_mem_words;
extern int g_sr_last_writer_enabled;

void sr_add_mem_watch(uint32_t start, uint32_t end, const char *label);
void sr_add_value_watch(uint32_t value, const char *label);
void sr_debug_init_watches(void);

/* Bounded, opt-in provenance for transient guest buffers.  Unlike a normal
 * watchpoint, this records the most recent generated-code PC for each recently
 * written address so a later consumer (notably the GE) can identify the write
 * that produced data in a rotating arena. */
void sr_last_writer_reset(void);
void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc);
int sr_find_last_writer(uint32_t addr, uint32_t width,
                        uint32_t *write_addr, uint32_t *write_width,
                        uint32_t *value, uint32_t *pc);

static inline int sr_check_mem_watch(uint32_t addr, uint32_t val, int write, uint32_t pc) {
    /* An explicitly configured watch is already an opt-in diagnostic.  Keep the
       common no-watch path cheap, but do not require the broader SR_DEBUG=MEM
       category as a second switch (profiles may intentionally clear it). */
    if (g_sr_mem_watch_count == 0) return 0;
    for (int i = 0; i < g_sr_mem_watch_count; i++) {
        const SrMemWatch *watch = &g_sr_mem_watches[i];
        int matched = watch->match_value
            ? val == watch->value
            : addr >= watch->start && addr < watch->end;
        if (matched) {
            fprintf(stderr, "%s[%s]: %s addr=0x%08x val=0x%08x pc=0x%08x\n",
                    watch->match_value ? "MEM_VALUE_WATCH" : "MEM_WATCH",
                    g_sr_mem_watches[i].label,
                    write ? "WRITE" : "READ",
                    addr, val, pc);
            return 1;
        }
    }
    return 0;
}

#endif // PSP_RECOMP_RT_DEBUG_H
