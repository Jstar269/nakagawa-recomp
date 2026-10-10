// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors
//
// Debug framework implementation. Defines global state and memory watch infrastructure.

#define _CRT_SECURE_NO_WARNINGS
#include "debug.h"
#include "watchpoints_file.h"
#include <string.h>

// Global debug mask (set once at startup)
uint32_t g_sr_debug = 0;

// Memory watch table
SrMemWatch g_sr_mem_watches[SR_MAX_MEM_WATCHES];
int g_sr_mem_watch_count = 0;
int g_sr_metadata_watch = 0;
uint32_t g_sr_mem_watch_context_pc = 0;
unsigned g_sr_mem_watch_context_limit = 0;
unsigned g_sr_mem_watch_context_count = 0;
int g_sr_mem_watch_context_fpr = -1;
uint32_t g_sr_mem_watch_context_fpr_value = 0;
uint32_t g_sr_store_context_pc = 0;
unsigned g_sr_store_context_limit = 0;
unsigned g_sr_store_context_count = 0;
int g_sr_store_context_mem_gpr = -1;
uint32_t g_sr_store_context_mem_offset = 0;
unsigned g_sr_store_context_mem_words = 0;
int g_sr_last_writer_enabled = 0;

/* Four-way buckets make address collisions very unlikely without turning an
 * opt-in diagnostic into an unbounded route trace.  Entries are replaced by
 * age within their bucket, while exact-address rewrites update in place. */
#define SR_LAST_WRITER_BUCKETS 4096u
#define SR_LAST_WRITER_WAYS 4u
typedef struct {
    uint32_t addr;
    uint32_t value;
    uint32_t pc;
    uint32_t serial;
    uint8_t width;
    uint8_t valid;
} SrLastWriter;

static SrLastWriter s_last_writers[SR_LAST_WRITER_BUCKETS][SR_LAST_WRITER_WAYS];
static uint32_t s_last_writer_serial;

static uint32_t sr_last_writer_bucket(uint32_t addr) {
    return (addr * 2654435761u) & (SR_LAST_WRITER_BUCKETS - 1u);
}

static uint32_t sr_last_writer_phys(uint32_t addr) {
    /* Match SR_PHYS without depending on recomp.h (which includes debug.h).
     * Generated code commonly writes through a 0x4xxxxxxx alias while the GE
     * consumes the same bytes through 0x0xxxxxxx. */
    return addr & 0x1fffffffu;
}

void sr_last_writer_reset(void) {
    memset(s_last_writers, 0, sizeof(s_last_writers));
    s_last_writer_serial = 0;
}

void sr_note_mem_write(uint32_t addr, uint32_t width, uint32_t value, uint32_t pc) {
    if (!g_sr_last_writer_enabled || width == 0 || width > 4) return;
    addr = sr_last_writer_phys(addr);
    SrLastWriter *bucket = s_last_writers[sr_last_writer_bucket(addr)];
    SrLastWriter *slot = NULL;
    for (unsigned i = 0; i < SR_LAST_WRITER_WAYS; i++) {
        if (bucket[i].valid && bucket[i].addr == addr) {
            slot = &bucket[i];
            break;
        }
        if (!slot || !bucket[i].valid || bucket[i].serial < slot->serial) {
            slot = &bucket[i];
        }
    }
    s_last_writer_serial++;
    if (s_last_writer_serial == 0) {
        /* A route would need more than four billion tracked writes to reach
         * this point; reset rather than make wrapped ages ambiguous. */
        sr_last_writer_reset();
        s_last_writer_serial = 1;
    }
    slot->addr = addr;
    slot->width = (uint8_t)width;
    slot->value = value;
    slot->pc = pc;
    slot->serial = s_last_writer_serial;
    slot->valid = 1;
}

int sr_find_last_writer(uint32_t addr, uint32_t width,
                        uint32_t *write_addr, uint32_t *write_width,
                        uint32_t *value, uint32_t *pc) {
    if (!g_sr_last_writer_enabled || width == 0 || width > 4) return 0;
    addr = sr_last_writer_phys(addr);
    SrLastWriter *best = NULL;
    uint32_t first = addr >= 3u ? addr - 3u : 0u;
    uint64_t query_end = (uint64_t)addr + width;
    uint64_t last = query_end - 1u;
    if (last > UINT32_MAX) last = UINT32_MAX;
    for (uint64_t start64 = first; start64 <= last; start64++) {
        uint32_t start = (uint32_t)start64;
        SrLastWriter *bucket = s_last_writers[sr_last_writer_bucket(start)];
        for (unsigned i = 0; i < SR_LAST_WRITER_WAYS; i++) {
            SrLastWriter *entry = &bucket[i];
            if (!entry->valid || entry->addr != start) continue;
            uint64_t write_end = (uint64_t)entry->addr + entry->width;
            if ((uint64_t)entry->addr < query_end && write_end > addr &&
                (!best || entry->serial > best->serial)) {
                best = entry;
            }
        }
    }
    if (!best) return 0;
    if (write_addr) *write_addr = best->addr;
    if (write_width) *write_width = best->width;
    if (value) *value = best->value;
    if (pc) *pc = best->pc;
    return 1;
}

void sr_add_mem_watch(uint32_t start, uint32_t end, const char *label) {
    for (int i = 0; i < g_sr_mem_watch_count; i++) {
        if (!g_sr_mem_watches[i].match_value &&
            g_sr_mem_watches[i].start == start && g_sr_mem_watches[i].end == end) {
            return;
        }
    }
    if (g_sr_mem_watch_count < SR_MAX_MEM_WATCHES) {
        g_sr_mem_watches[g_sr_mem_watch_count].start = start;
        g_sr_mem_watches[g_sr_mem_watch_count].end = end;
        g_sr_mem_watches[g_sr_mem_watch_count].value = 0;
        g_sr_mem_watches[g_sr_mem_watch_count].label = label;
        g_sr_mem_watches[g_sr_mem_watch_count].match_value = 0;
        g_sr_mem_watch_count++;
    } else {
        fprintf(stderr, "sr_add_mem_watch: capacity exceeded (%d max) -- "
                "requested [%08x..%08x] label='%s' silently dropped\n",
                SR_MAX_MEM_WATCHES, start, end, label ? label : "(null)");
    }
}

void sr_add_value_watch(uint32_t value, const char *label) {
    if (g_sr_mem_watch_count < SR_MAX_MEM_WATCHES) {
        g_sr_mem_watches[g_sr_mem_watch_count].start = 0;
        g_sr_mem_watches[g_sr_mem_watch_count].end = 0;
        g_sr_mem_watches[g_sr_mem_watch_count].value = value;
        g_sr_mem_watches[g_sr_mem_watch_count].label = label;
        g_sr_mem_watches[g_sr_mem_watch_count].match_value = 1;
        g_sr_mem_watch_count++;
    } else {
        fprintf(stderr, "sr_add_value_watch: capacity exceeded (%d max) -- "
                "requested value=%08x label='%s' silently dropped\n",
                SR_MAX_MEM_WATCHES, value, label ? label : "(null)");
    }
}

static char *sr_strdup(const char *s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char *d = (char *)malloc(len + 1);
    if (d) {
        memcpy(d, s, len + 1);
    }
    return d;
}

void sr_debug_init_watches(void) {
    g_sr_mem_watch_count = 0;
    g_sr_mem_watch_context_pc = 0;
    g_sr_mem_watch_context_limit = 0;
    g_sr_mem_watch_context_count = 0;
    g_sr_mem_watch_context_fpr = -1;
    g_sr_mem_watch_context_fpr_value = 0;
    g_sr_store_context_pc = 0;
    g_sr_store_context_limit = 0;
    g_sr_store_context_count = 0;
    g_sr_store_context_mem_gpr = -1;
    g_sr_store_context_mem_offset = 0;
    g_sr_store_context_mem_words = 0;
    g_sr_last_writer_enabled = 0;
    sr_last_writer_reset();

    const char *last_writer = getenv("SR_TRACK_LAST_WRITER");
    const char *ge_arm_rect = getenv("SR_GE_ARM_RECT");
    if ((last_writer && strcmp(last_writer, "0") != 0) || ge_arm_rect) {
        g_sr_last_writer_enabled = 1;
    }

    // 1. Try reading from watchpoints.json — the derived runtime artifact
    // (issue #188). The bounded parser accepts the versioned envelope and
    // the legacy bare-array form, and fails closed on anything else.
    // SR_WATCHPOINTS_FILE overrides the path, defaulting to the CWD-relative
    // watchpoints.json.
    {
        SrWatchpointEntry entries[SR_MAX_MEM_WATCHES];
        char wp_err[160];
        const char *wp_path = getenv("SR_WATCHPOINTS_FILE");
        if (!wp_path || wp_path[0] == '\0') wp_path = "watchpoints.json";
        int n = sr_parse_watchpoints_file(wp_path, entries, SR_MAX_MEM_WATCHES,
                                          wp_err, sizeof(wp_err));
        if (n > 0) {
            for (int i = 0; i < n; i++) {
                char *dup_lbl = sr_strdup(entries[i].label);
                if (dup_lbl) {
                    sr_add_mem_watch(entries[i].start, entries[i].end, dup_lbl);
                }
            }
        } else if (n < 0) {
            fprintf(stderr, "sr_debug_init_watches: watchpoints.json not loaded: %s\n", wp_err);
        }
    }

    // 2. Fallback/override: parse from environment variables SR_WATCH_0 ... SR_WATCH_15
    for (int i = 0; i < SR_MAX_MEM_WATCHES; i++) {
        char env_name[32];
        sprintf(env_name, "SR_WATCH_%d", i);
        char *env_val = getenv(env_name);
        if (env_val) {
            uint32_t st_val = 0, end_val = 0;
            char lbl_val[128] = {0};
            if (sscanf(env_val, "0x%x,0x%x,%127[^,]", &st_val, &end_val, lbl_val) == 3 ||
                sscanf(env_val, "%u,%u,%127[^,]", &st_val, &end_val, lbl_val) == 3) {
                char *dup_lbl = sr_strdup(lbl_val);
                if (dup_lbl) {
                    sr_add_mem_watch(st_val, end_val, dup_lbl);
                }
            }
        }
    }

    // 3. Value watches find writes whose destination address is dynamic, such as
    // transient vertex arenas. Syntax: SR_VALUE_WATCH_N=0xVALUE,label.
    for (int i = 0; i < SR_MAX_MEM_WATCHES; i++) {
        char env_name[32];
        sprintf(env_name, "SR_VALUE_WATCH_%d", i);
        char *env_val = getenv(env_name);
        if (env_val) {
            uint32_t value = 0;
            char lbl_val[128] = {0};
            if (sscanf(env_val, "0x%x,%127[^,]", &value, lbl_val) == 2 ||
                sscanf(env_val, "%u,%127[^,]", &value, lbl_val) == 2) {
                char *dup_lbl = sr_strdup(lbl_val);
                if (dup_lbl) {
                    sr_add_value_watch(value, dup_lbl);
                }
            }
        }
    }

    // 4. Optionally emit a bounded CpuState snapshot when a matched memory/value
    // watch originates at one exact guest PC. The formatting lives in recomp.h,
    // after CpuState is defined; this file owns only configuration/state.
    char *context_pc = getenv("SR_WATCH_CONTEXT_PC");
    if (context_pc) {
        char *end = NULL;
        unsigned long parsed = strtoul(context_pc, &end, 0);
        if (end != context_pc && *end == '\0' && parsed <= 0xffffffffUL) {
            g_sr_mem_watch_context_pc = (uint32_t)parsed;
            g_sr_mem_watch_context_limit = 1;
            char *context_limit = getenv("SR_WATCH_CONTEXT_LIMIT");
            if (context_limit) {
                end = NULL;
                parsed = strtoul(context_limit, &end, 0);
                if (end != context_limit && *end == '\0' && parsed > 0 && parsed <= 1024UL) {
                    g_sr_mem_watch_context_limit = (unsigned)parsed;
                }
            }
            char *context_fpr = getenv("SR_WATCH_CONTEXT_FPR");
            if (context_fpr) {
                char *index_end = NULL;
                unsigned long index = strtoul(context_fpr, &index_end, 0);
                if (index_end != context_fpr && *index_end == ',' && index < 32UL) {
                    char *value_end = NULL;
                    unsigned long value = strtoul(index_end + 1, &value_end, 0);
                    if (value_end != index_end + 1 && *value_end == '\0' && value <= 0xffffffffUL) {
                        g_sr_mem_watch_context_fpr = (int)index;
                        g_sr_mem_watch_context_fpr_value = (uint32_t)value;
                    } else {
                        fprintf(stderr, "SR_WATCH_CONTEXT_FPR: invalid value '%s'\n", context_fpr);
                    }
                } else {
                    fprintf(stderr, "SR_WATCH_CONTEXT_FPR: invalid register '%s'\n", context_fpr);
                }
            }
        } else {
            fprintf(stderr, "SR_WATCH_CONTEXT_PC: invalid guest PC '%s'\n", context_pc);
        }
    }

    // 5. Independently snapshot stores from one exact generated-code PC. This
    // avoids broad value-watch logging when a common value (for example a UI
    // coordinate) is written throughout a long route.
    char *store_context_pc = getenv("SR_STORE_CONTEXT_PC");
    if (store_context_pc) {
        char *end = NULL;
        unsigned long parsed = strtoul(store_context_pc, &end, 0);
        if (end != store_context_pc && *end == '\0' && parsed <= 0xffffffffUL) {
            g_sr_store_context_pc = (uint32_t)parsed;
            g_sr_store_context_limit = 1;
            char *store_context_limit = getenv("SR_STORE_CONTEXT_LIMIT");
            if (store_context_limit) {
                end = NULL;
                parsed = strtoul(store_context_limit, &end, 0);
                if (end != store_context_limit && *end == '\0' && parsed > 0 && parsed <= 1024UL) {
                    g_sr_store_context_limit = (unsigned)parsed;
                }
            }
            char *store_context_mem = getenv("SR_STORE_CONTEXT_MEM");
            if (store_context_mem) {
                unsigned reg = 0, words = 0;
                int offset = 0;
                char trailing = '\0';
                if (sscanf(store_context_mem, "%u,%i,%u%c", &reg, &offset,
                           &words, &trailing) == 3 && reg < 32u && words > 0u && words <= 32u) {
                    g_sr_store_context_mem_gpr = (int)reg;
                    g_sr_store_context_mem_offset = (uint32_t)offset;
                    g_sr_store_context_mem_words = words;
                } else {
                    fprintf(stderr, "SR_STORE_CONTEXT_MEM: invalid value '%s'\n", store_context_mem);
                }
            }
        } else {
            fprintf(stderr, "SR_STORE_CONTEXT_PC: invalid guest PC '%s'\n", store_context_pc);
        }
    }

}

/* ---- SR_NAN_TRAP reporter (see the SR_NAN_TRAP block in recomp.h) ----------
 *
 * Compiled only when -DSR_NAN_TRAP is defined. Without it this file contributes
 * nothing at all, so an untrapped build is byte-identical and link-identical.
 *
 * Classification is on the raw IEEE-754 bits, not on a floating comparison: the
 * predicate "was every input finite" must not itself be a guest-sensitive FP
 * operation that ambient host DAZ could decide differently (the same argument
 * that keeps sr_fpu_mul_s off a floating inf*0 precheck). A word is finite when
 * its exponent field is not all ones. */
#ifdef SR_NAN_TRAP
/* Only for the SR_NAN_TRAP_DEFAULT_LIMIT contract and the two prototypes: the
 * trap reporter is diagnostics, and debug.c stays the diagnostics home. */
#include "recomp.h"

static int s_nan_trap_armed = -1;
static int s_nan_trap_limit = -1;
static int s_nan_trap_hits = 0;

static void sr_nan_trap_configure(void) {
    if (s_nan_trap_armed >= 0) return;
    s_nan_trap_armed = 1;
    s_nan_trap_limit = SR_NAN_TRAP_DEFAULT_LIMIT;
    const char *text = getenv("SR_NAN_TRAP_LIMIT");
    if (text && text[0]) {
        char *end = NULL;
        long parsed = strtol(text, &end, 10);
        if (end == text || *end != '\0' || parsed < 0 || parsed > 1000000L) {
            fprintf(stderr, "NAN_TRAP: invalid SR_NAN_TRAP_LIMIT '%s' (using %d)\n",
                    text, s_nan_trap_limit);
        } else {
            s_nan_trap_limit = (int)parsed;
        }
    }
    fprintf(stderr, "NAN_TRAP: armed limit=%d (first N finite->non-finite results)\n",
            s_nan_trap_limit);
}

/* 1 when the word is NaN or +/-Inf, 0 when it is finite. */
static int sr_nan_trap_nonfinite(float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    return (int)((bits & 0x7F800000u) == 0x7F800000u);
}

/* Render one value the way the guest sees it: a finite float as a number, a NaN
 * as `nan`, an infinity as `inf`/`-inf`. Never `-nan(ind)`, which is a host
 * printf spelling and not a stable token to grep. */
static void sr_nan_trap_print(FILE *fp, const float *v, int n) {
    for (int i = 0; i < n; i++) {
        uint32_t bits;
        memcpy(&bits, &v[i], sizeof(bits));
        if (i) fputc(',', fp);
        if ((bits & 0x7FFFFFFFu) == 0x7F800000u) {
            fputs((bits & 0x80000000u) ? "-inf" : "inf", fp);
        } else if ((bits & 0x7F800000u) == 0x7F800000u) {
            fputs("nan", fp);
        } else {
            fprintf(fp, "%.9g", (double)v[i]);
        }
    }
}

static int sr_nan_trap_budget(void) {
    sr_nan_trap_configure();
    if (s_nan_trap_hits >= s_nan_trap_limit) {
        if (s_nan_trap_hits == s_nan_trap_limit) {
            s_nan_trap_hits++;
            fprintf(stderr, "NAN_TRAP: limit=%d reached; further reports suppressed\n",
                    s_nan_trap_limit);
        }
        return 0;
    }
    s_nan_trap_hits++;
    return 1;
}

/* SR_NAN_TRAP_CONTEXT=1: after each report, the registers a reader needs to find the
 * instruction's inputs in guest memory (return address, value and argument registers,
 * two saved registers, f12) and the four words each of v0 and a0..a3 points at when that
 * span is readable. A quaternion slerp whose vasin reports a NaN, for example, has its
 * two input quaternions behind a1 and v0 at that point. Off by default; the probe reads
 * the scheduler's current CpuState, so a report from outside guest execution prints
 * nothing. */
static int s_nan_trap_ctx = -1;
static void sr_nan_trap_context(void) {
    if (s_nan_trap_ctx < 0) {
        const char *p = getenv("SR_NAN_TRAP_CONTEXT");
        s_nan_trap_ctx = (p && p[0] && strcmp(p, "0") != 0) ? 1 : 0;
    }
    if (!s_nan_trap_ctx || !s_cpu) return;
    const CpuState *c = s_cpu;
    fprintf(stderr, "NAN_TRAP_CTX ra=0x%08x v0=0x%08x a0=0x%08x a1=0x%08x a2=0x%08x a3=0x%08x "
            "t0=0x%08x s0=0x%08x s1=0x%08x f12=%.9g\n",
            c->r[31], c->r[2], c->r[4], c->r[5], c->r[6], c->r[7], c->r[8], c->r[16], c->r[17],
            (double)c->f[12]);
    static const struct { const char *name; int reg; } ptrs[] = {
        { "v0", 2 }, { "a0", 4 }, { "a1", 5 }, { "a2", 6 }, { "a3", 7 },
    };
    for (size_t i = 0; i < sizeof(ptrs) / sizeof(ptrs[0]); i++) {
        uint32_t a = c->r[ptrs[i].reg];
        if ((a & 3u) != 0u || !sr_guest_span_readable(a, 16u)) continue;
        float v[4];
        memcpy(v, SR_HOST(a), sizeof(v));
        fprintf(stderr, "NAN_TRAP_CTX %s@0x%08x=[", ptrs[i].name, a);
        sr_nan_trap_print(stderr, v, 4);
        fputs("]\n", stderr);
    }
}

void sr_nan_trap_note(uint32_t pc, const char *op, uint32_t fd,
                      float out, const float *in, int nin) {
    if (!sr_nan_trap_nonfinite(out)) return;
    for (int i = 0; i < nin; i++)
        if (sr_nan_trap_nonfinite(in[i])) return;   /* propagation, not origin */
    if (!sr_nan_trap_budget()) return;
    const float out1[1] = { out };
    fprintf(stderr, "NAN_TRAP pc=0x%08x op=%s dst=f%u vbl=%u in=[", pc, op, fd,
            sr_audio_vbl());
    sr_nan_trap_print(stderr, in, nin);
    fputs("] out=[", stderr);
    sr_nan_trap_print(stderr, out1, 1);
    fputs("]\n", stderr);
    sr_nan_trap_context();
}

void sr_nan_trap_note_v(uint32_t pc, const char *op, uint32_t vd,
                        const float *out, int nout,
                        const float *in1, int nin1,
                        const float *in2, int nin2) {
    int produced = 0;
    for (int i = 0; i < nout; i++) produced |= sr_nan_trap_nonfinite(out[i]);
    if (!produced) return;
    for (int i = 0; i < nin1; i++)
        if (sr_nan_trap_nonfinite(in1[i])) return;   /* propagation, not origin */
    for (int i = 0; i < nin2; i++)
        if (sr_nan_trap_nonfinite(in2[i])) return;   /* propagation, not origin */
    if (!sr_nan_trap_budget()) return;
    fprintf(stderr, "NAN_TRAP pc=0x%08x op=%s dst=v%u vbl=%u in=[", pc, op, vd,
            sr_audio_vbl());
    sr_nan_trap_print(stderr, in1, nin1);
    if (nin2 > 0) fputc(',', stderr);
    sr_nan_trap_print(stderr, in2, nin2);
    fputs("] out=[", stderr);
    sr_nan_trap_print(stderr, out, nout);
    fputs("]\n", stderr);
    sr_nan_trap_context();
}
#endif /* SR_NAN_TRAP */
