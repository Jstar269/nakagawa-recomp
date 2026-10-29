// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors

#ifndef NAKAGAWA_FLIGHT_RECORDER_H
#define NAKAGAWA_FLIGHT_RECORDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SR_FLIGHT_CLASS_HLE = 1u << 0,
    SR_FLIGHT_CLASS_UNSUPPORTED = 1u << 1,
    SR_FLIGHT_CLASS_SCHED = 1u << 2,
    SR_FLIGHT_CLASS_PRX = 1u << 3,
    SR_FLIGHT_CLASS_FAULT = 1u << 4,
    SR_FLIGHT_CLASS_FATAL = 1u << 5,
    SR_FLIGHT_CLASS_MEDIA = 1u << 6,
    SR_FLIGHT_CLASS_GE = 1u << 7,
    SR_FLIGHT_CLASS_PRESENT = 1u << 8,
    /* Value-changing guest stores inside the SR_WATCH range (schema 7). */
    SR_FLIGHT_CLASS_WATCH = 1u << 9,
    SR_FLIGHT_CLASS_ALL = (1u << 10) - 1u,
    SR_FLIGHT_MAX_EVENTS = 4096u,
    SR_FLIGHT_MAX_THREADS = 128u,
    /* The watch class keeps a byte shadow of the watched range to tell a change from a
     * rewrite of the same value, so it accepts an SR_WATCH range of at most this length. */
    SR_FLIGHT_WATCH_SHADOW_MAX = 64u,
    SR_FLIGHT_SCHEMA_VERSION = 7u
};

/* ---- Trigger modes (schema 7) ----
 * SR_FLIGHT_TRIGGER selects at most one mode; with it unset the mode is none and the recorder
 * behaves exactly as before. A mode matches a RECORDED event, so it only sees events of the
 * enabled classes, and it never matches an event a terminal path (fatal, fault, hang, an NID
 * with no handler) records: those keep their own trigger. Checking a mode is arithmetic on
 * the event being recorded; it never sleeps, yields, takes a lock or reads guest memory.
 *
 *   pc:ADDR                    the first event whose guest pc operand is ADDR (an HLE import
 *                              call site, a named refusal, a GE list dequeue call site, a NaN
 *                              origin or a watch change)
 *   ge-draw:N                  the Nth GE draw event (class ge, kind 25)
 *   watch                      the first watch-change event (needs the watch class)
 *   checkpoint:KIND:N:AFTER    the Nth event of KIND, followed by AFTER more events; AFTER is
 *                              below the ring limit, so the ring keeps limit-1-AFTER events
 *                              before the checkpoint
 *
 * When a mode matches, the terminal becomes "trigger" (kind: the matched event's kind, arg0:
 * the mode's argument, sequence: the matched event). The record freezes at the match, or after
 * the AFTER window of a checkpoint, and the bundle is written then; the run keeps going. */
enum {
    SR_FLIGHT_TRIGGER_NONE = 0u,
    SR_FLIGHT_TRIGGER_PC = 1u,
    SR_FLIGHT_TRIGGER_GE_DRAW = 2u,
    SR_FLIGHT_TRIGGER_WATCH = 3u,
    SR_FLIGHT_TRIGGER_CHECKPOINT = 4u
};

/* ---- Terminal thread snapshot (schema 6) ----
 * Every bundle carries one entry per guest thread that exists when it is written, so a
 * thread that blocked long before the retained event window still shows what it waits on.
 * The scheduler owns the thread table; it registers a source once at init and the recorder
 * calls it while writing a bundle. The recorder never links against the scheduler: with no
 * source registered the bundle writes an empty "threads" array. Every field is a scalar the
 * scheduler holds; nothing is read from guest memory. */
enum {
    SR_FLIGHT_THREAD_DORMANT = 0u,
    SR_FLIGHT_THREAD_READY = 1u,
    SR_FLIGHT_THREAD_RUNNING = 2u,
    SR_FLIGHT_THREAD_WAIT_DELAY = 3u,
    SR_FLIGHT_THREAD_WAIT_OBJECT = 4u
};

/* Why a waiting thread is parked, in the classification sched_dump_threads() prints. */
enum {
    SR_FLIGHT_WHY_NONE = 0u,
    SR_FLIGHT_WHY_SLEEP = 1u,
    SR_FLIGHT_WHY_DELAY = 2u,
    SR_FLIGHT_WHY_VBLANK = 3u,
    SR_FLIGHT_WHY_CTRL = 4u,
    SR_FLIGHT_WHY_THREAD_END = 5u,
    SR_FLIGHT_WHY_OBJECT = 6u
};

typedef struct {
    uint32_t uid;
    int32_t priority;        /* current priority */
    uint32_t state;          /* SR_FLIGHT_THREAD_* */
    uint32_t suspended;      /* 1 while sceKernelSuspendThread is in effect */
    uint32_t why;            /* SR_FLIGHT_WHY_* */
    uint32_t wait_type;      /* PSP waitType as ReferThreadStatus reports it; 0 when not waiting */
    uint32_t wait_object;    /* waited object; 0 when not in an object wait */
    uint32_t has_block_nid;  /* 0: the last block happened outside any import */
    uint32_t block_nid;      /* import NID the thread last blocked in */
    uint32_t pc;             /* saved pc, or the live pc of the running thread */
    uint32_t ra;             /* saved r[31], or the live one of the running thread */
    uint32_t has_waited;     /* 0: not waiting, or the wait start is unknown */
    uint64_t waited_us;      /* virtual microseconds since the current wait began */
    uint32_t has_deadline;   /* 0: no wait, or an untimed wait */
    uint64_t deadline_us;    /* virtual microseconds left before a timed wait expires */
} SrFlightThread;

/* Fills *out for the index-th existing thread and returns 1, or returns 0 when index is past
 * the last one. Indices run 0, 1, 2, ... with no gaps. */
typedef int (*SrFlightThreadSource)(uint32_t index, SrFlightThread *out);

enum {
    SR_FLIGHT_KIND_HLE_IMPORT = 1u,
    SR_FLIGHT_KIND_UNSUPPORTED_NID = 2u,
    SR_FLIGHT_KIND_SCHED_PICK = 3u,
    SR_FLIGHT_KIND_SCHED_BLOCK = 4u,
    SR_FLIGHT_KIND_SCHED_WAKE = 5u,
    SR_FLIGHT_KIND_PRX_LOAD = 6u,
    SR_FLIGHT_KIND_PRX_START = 7u,
    SR_FLIGHT_KIND_PRX_STOP = 8u,
    SR_FLIGHT_KIND_PRX_UNLOAD = 9u,
    SR_FLIGHT_KIND_FAULT_EXCEPTION = 10u,
    SR_FLIGHT_KIND_FATAL_BREAK = 11u,
    SR_FLIGHT_KIND_FATAL_RAW_SYSCALL = 12u,
    SR_FLIGHT_KIND_FATAL_DISPATCH = 13u,
    SR_FLIGHT_KIND_FATAL_UNIMPLEMENTED = 14u,
    SR_FLIGHT_KIND_FATAL_CPU_FLOW = 15u,
    SR_FLIGHT_KIND_FATAL_IMPORT = 16u,
    SR_FLIGHT_KIND_FATAL_HOST = 17u,
    SR_FLIGHT_KIND_MEDIA_TEST_MILESTONE = 18u,
    /* A production media stream the demuxer refused.  arg0/arg1 are the low and high
     * halves of the source offset of the refused element; the reason itself is named in
     * the always-on stderr boundary the same code prints.  This is a project-owned
     * diagnostic label, not a PSP hardware event name. */
    SR_FLIGHT_KIND_MEDIA_STREAM_REJECTED = 19u,
    SR_FLIGHT_KIND_GE_LIST_ENQUEUE = 20u,
    SR_FLIGHT_KIND_GE_LIST_DEQUEUE = 21u,
    SR_FLIGHT_KIND_GE_LIST_SYNC = 22u,
    SR_FLIGHT_KIND_GE_DRAW_SYNC = 23u,
    SR_FLIGHT_KIND_GE_LIST_STALL_UPDATE = 24u,
    SR_FLIGHT_KIND_GE_DRAW = 25u,
    SR_FLIGHT_KIND_GE_LIST_FINISH = 26u,
    SR_FLIGHT_KIND_GE_FINISH_COMMAND = 27u,
    SR_FLIGHT_KIND_PRESENT_SET_FRAMEBUF = 28u,
    SR_FLIGHT_KIND_PRESENT_FRAME = 29u,
    /* A NaN/Inf origin reported by SR_NAN_TRAP (issue #69): a FPU or VFPU result that went
     * non-finite from all-finite operands, recorded in the fault class with no trigger, so
     * a run keeps going after it. arg0 is the guest pc; arg1 packs the domain
     * (SR_FLIGHT_NAN_DOMAIN_*, bits 0-7), the result class (SR_FLIGHT_NAN_RESULT_*, bits
     * 8-15) and the destination register (bits 16-23); arg2 is the lane, the index of the
     * first non-finite word in the reported output; arg3 is the VBLANK count. The reporter
     * applies the same SR_NAN_TRAP_LIMIT budget as its printed line. */
    SR_FLIGHT_KIND_NAN_ORIGIN = 30u,
    /* A guest store inside the SR_WATCH range that changed at least one byte (watch class,
     * schema 7). arg0 is the storing pc, arg1 the store address, arg2 packs the store width
     * (bits 0-7) and SR_FLIGHT_WATCH_KNOWN (bit 8: every watched byte it covers had a known
     * earlier value, so this is a change rather than the first write the recorder saw), and
     * arg3 is the stored value. A store that rewrites the same bytes records nothing. */
    SR_FLIGHT_KIND_WATCH_CHANGE = 31u
};

enum { SR_FLIGHT_WATCH_KNOWN = 1u << 8 };

/* Operands of SR_FLIGHT_KIND_NAN_ORIGIN (see above). */
enum {
    SR_FLIGHT_NAN_DOMAIN_FPU = 1u,
    SR_FLIGHT_NAN_DOMAIN_VFPU = 2u,
    SR_FLIGHT_NAN_RESULT_NAN = 1u,
    SR_FLIGHT_NAN_RESULT_POS_INF = 2u,
    SR_FLIGHT_NAN_RESULT_NEG_INF = 3u
};

/* Synthetic media-selftest labels only. They describe source-owned fixture
 * milestones; they are not claims about PSP hardware event names or timing. */
enum {
    SR_FLIGHT_MEDIA_TEST_SOURCE_READY = 1u,
    SR_FLIGHT_MEDIA_TEST_PRODUCER_OPEN = 2u,
    SR_FLIGHT_MEDIA_TEST_AU_READY = 3u,
    SR_FLIGHT_MEDIA_TEST_FRAME_READY = 4u,
    SR_FLIGHT_MEDIA_TEST_EOF = 5u
};

enum {
    SR_FLIGHT_TERMINAL_RUNNING = 0u,
    SR_FLIGHT_TERMINAL_EXIT = 1u,
    SR_FLIGHT_TERMINAL_FATAL = 2u,
    SR_FLIGHT_TERMINAL_UNSUPPORTED_NID = 3u,
    SR_FLIGHT_TERMINAL_BUDGET = 4u,
    SR_FLIGHT_TERMINAL_HANG = 5u,
    SR_FLIGHT_TERMINAL_TRIGGER = 6u
};

typedef struct {
    uint32_t schema_version;
    uint64_t sequence;
    uint32_t event_class;
    uint32_t kind;
    uint32_t arg0;
    uint32_t arg1;
    uint32_t arg2;
    uint32_t arg3;
    uint32_t arguments[4];
    uint32_t has_return;
    uint32_t return_value;
} SrFlightEvent;

typedef struct {
    uint32_t enabled_classes;
    uint32_t limit;
    uint32_t retained;
    uint64_t recorded;
    uint64_t dropped;
    uint64_t trigger_count;
    uint32_t dump_count;
    uint32_t terminal_reason;
    uint32_t terminal_kind;
    uint32_t terminal_arg;
    uint64_t terminal_sequence;
    /* Named refusals: calls the guest was told were refused and kept running after. */
    uint64_t refusal_count;
    uint32_t refusal_first_nid;
    uint32_t refusal_first_pc;
    uint64_t refusal_first_sequence;
    uint32_t refusal_distinct;
    uint64_t refusal_unlisted;
    /* Trigger mode (schema 7): SR_FLIGHT_TRIGGER_*, the sequence of the event it matched
     * (0 until it matches), and the events still owed to an open checkpoint window. */
    uint32_t trigger_mode;
    uint64_t trigger_match_sequence;
    uint32_t trigger_after_remaining;
} SrFlightSnapshot;

/* ---- SR_TRACE_PC: an address window over the instruction trace ----
 * The full trace is one line per guest instruction, which no run of any length
 * can afford to write. SR_TRACE_PC=LO:HI keeps only the records whose guest pc
 * is inside the window and adds the ABSOLUTE value of the registers SR_TRACE_V
 * (VFPU) and SR_TRACE_F (FPU) name, because a window cannot reconstruct a value
 * written before it. SR_TRACE_LIMIT stops the stream after that many records
 * and disarms the gate, so the rest of the run executes uninstrumented. Every
 * delivered vblank is marked, which places each record on the frame timeline.
 *
 * This is a diagnostic stream, not the byte-comparable oracle trace: a record is
 * the canonical line the trace writer already rendered, followed by the absolute
 * values. It is owned by the diagnostic recorder because every host of the
 * runtime links that file -- the host probes the window before guest execution,
 * and the GE's vblank tick marks the frame timeline. Nothing here
 * is active unless SR_TRACE_PC names a window. The index list is one shared type
 * in both branches below, so a host that does not link the recorder still
 * compiles the trace writer against the same declaration. */
#define SR_TRACE_WINDOW_MAX_INDEX 16u
typedef struct {
    unsigned vn, fn;                      /* how many absolute indices are named */
    uint8_t v[SR_TRACE_WINDOW_MAX_INDEX]; /* VFPU indices, physical v[] order */
    uint8_t f[SR_TRACE_WINDOW_MAX_INDEX]; /* FPU indices */
} SrTraceWindowIndices;

#if defined(SR_FLIGHT_RECORDER_LINKED) && !defined(SR_FLIGHT_RECORDER_STANDALONE)

void sr_trace_window_configure(void);   /* lazy env probe; host start and vblank hooks */
void sr_trace_note_frame(uint32_t frame);
int  sr_trace_window_armed(void);
int  sr_trace_window_begin_instruction(uint32_t pc);  /* 0 when outside the window */
const SrTraceWindowIndices *sr_trace_window_indices(void);
void sr_trace_window_record(uint32_t pc, const char *text);

/* ---- SR_WATCH: guest stores intersecting one address range ----
 * SR_WATCH=ADDR:LEN is probed once during runtime initialization. Store helpers
 * check the exported flag before entering the recorder, so the disabled path
 * adds only a predicted-false branch and no diagnostic call. */
extern int g_sr_watch_enabled;
#define SR_WATCH_ACTIVE() (g_sr_watch_enabled)
void sr_watch_configure(void);
void sr_watch_note_vblank(uint32_t vblank);
void sr_watch_store(uint32_t pc, uint32_t addr, uint32_t value, uint32_t width);
uint64_t sr_watch_match_count(void);
#define SR_WATCH_STORE_IF_ARMED(pc, addr, value, width) do { \
    if (__builtin_expect(g_sr_watch_enabled, 0)) \
        sr_watch_store((pc), (addr), (value), (width)); \
} while (0)

void sr_flight_init(void);
int sr_flight_class_enabled(uint32_t event_class);
void sr_flight_record(uint32_t event_class, uint32_t kind, uint32_t arg0, uint32_t arg1,
                      uint32_t arg2, uint32_t arg3);
uint64_t sr_flight_hle_import(uint32_t nid, uint32_t uid, uint32_t object_uid, uint32_t pc,
                              uint32_t ra);
void sr_flight_hle_arguments(uint64_t sequence, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                             uint32_t arg3);
void sr_flight_hle_return(uint64_t sequence, uint32_t return_value);
void sr_flight_unsupported(uint32_t nid, uint32_t error, uint32_t uid, uint32_t pc);
void sr_flight_unsupported_fatal(uint32_t nid, uint32_t uid, uint32_t pc);
void sr_flight_prx_load(uint32_t base, uint32_t result, uint32_t entry, uint32_t imports,
                        uint32_t exports);
void sr_flight_fatal(uint32_t kind, uint32_t pc, uint32_t detail, uint32_t aux);
void sr_flight_exit(uint32_t status);
void sr_flight_budget(uint32_t vblank);
void sr_flight_hang(uint32_t vblanks_without_frame, uint32_t limit);
void sr_flight_fault(uint32_t kind, uint32_t pc, uint32_t detail, uint32_t aux);
/* Host crash-filter entry: names SR_FLIGHT_KIND_FAULT_EXCEPTION with the host
 * exception code as detail and the guest fault address (or 0 when the fault
 * is outside guest memory) as aux, so the first host fault dumps a bounded
 * bundle whose fatal terminal carries the fault. Recording stays opt-in. */
void sr_flight_host_fault(uint32_t guest_pc, uint32_t exception_code, uint32_t guest_fault_addr);
/* SR_NAN_TRAP origin (SR_FLIGHT_KIND_NAN_ORIGIN, issue #320). Packs domain, result and dst
 * into arg1 as documented at the kind. It never triggers: a NaN origin neither freezes the
 * record nor ends the run, so the trap stays a diagnostic. */
void sr_flight_nan_origin(uint32_t guest_pc, uint32_t domain, uint32_t dst, uint32_t result,
                          uint32_t lane, uint32_t vbl);
void sr_flight_snapshot(SrFlightSnapshot *out);
int sr_flight_event_count(void);
int sr_flight_event_at(uint32_t index, SrFlightEvent *out);
/* Registers the thread table every later bundle snapshots (NULL clears it). */
void sr_flight_set_thread_source(SrFlightThreadSource source);

#if defined(SR_HLE_THREAD_SELFTEST) || defined(SR_SCHED_LIVENESS_TEST)
void sr_flight_test_reset(uint32_t enabled_classes, uint32_t limit);
void sr_flight_test_disable(void);
/* Arms one SR_FLIGHT_TRIGGER spec through the production parser after a test reset; returns 1
 * when it is accepted. A refused spec disables the recorder, as it does at startup. */
int sr_flight_test_trigger(const char *spec);
#endif

#define SR_FLIGHT_RECORD_CLASS(event_class, kind, arg0, arg1, arg2, arg3)                         \
    do {                                                                                          \
        if (sr_flight_class_enabled((event_class))) {                                            \
            sr_flight_record((event_class), (kind), (arg0), (arg1), (arg2), (arg3));             \
        }                                                                                         \
    } while (0)

#else

/* Not linked: the window stays disarmed and inert, so a host without the
 * recorder behaves exactly as it did before the probe existed. */
static inline void sr_trace_window_configure(void) {}
static inline void sr_trace_note_frame(uint32_t frame) { (void)frame; }
static inline int sr_trace_window_armed(void) { return 0; }
static inline int sr_trace_window_begin_instruction(uint32_t pc) { (void)pc; return 0; }
static inline const SrTraceWindowIndices *sr_trace_window_indices(void) { return 0; }
static inline void sr_trace_window_record(uint32_t pc, const char *text) {
    (void)pc; (void)text;
}
#define SR_WATCH_STORE_IF_ARMED(pc, addr, value, width) ((void)0)
#define SR_WATCH_ACTIVE() 0
static inline void sr_watch_configure(void) {}
static inline void sr_watch_note_vblank(uint32_t vblank) { (void)vblank; }
static inline void sr_watch_store(uint32_t pc, uint32_t addr, uint32_t value, uint32_t width) {
    (void)pc; (void)addr; (void)value; (void)width;
}
static inline uint64_t sr_watch_match_count(void) { return 0u; }

static inline void sr_flight_init(void) {}
static inline int sr_flight_class_enabled(uint32_t event_class) {
    (void)event_class;
    return 0;
}
static inline void sr_flight_record(uint32_t event_class, uint32_t kind, uint32_t arg0, uint32_t arg1,
                                    uint32_t arg2, uint32_t arg3) {
    (void)event_class;
    (void)kind;
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
}
static inline uint64_t sr_flight_hle_import(uint32_t nid, uint32_t uid, uint32_t object_uid,
                                            uint32_t pc, uint32_t ra) {
    (void)nid;
    (void)uid;
    (void)object_uid;
    (void)pc;
    (void)ra;
    return 0u;
}
static inline void sr_flight_hle_arguments(uint64_t sequence, uint32_t arg0, uint32_t arg1,
                                           uint32_t arg2, uint32_t arg3) {
    (void)sequence;
    (void)arg0;
    (void)arg1;
    (void)arg2;
    (void)arg3;
}
static inline void sr_flight_hle_return(uint64_t sequence, uint32_t return_value) {
    (void)sequence;
    (void)return_value;
}
static inline void sr_flight_unsupported(uint32_t nid, uint32_t error, uint32_t uid, uint32_t pc) {
    (void)nid;
    (void)error;
    (void)uid;
    (void)pc;
}
static inline void sr_flight_unsupported_fatal(uint32_t nid, uint32_t uid, uint32_t pc) {
    (void)nid;
    (void)uid;
    (void)pc;
}
static inline void sr_flight_prx_load(uint32_t base, uint32_t result, uint32_t entry, uint32_t imports,
                                     uint32_t exports) {
    (void)base;
    (void)result;
    (void)entry;
    (void)imports;
    (void)exports;
}
static inline void sr_flight_fatal(uint32_t kind, uint32_t pc, uint32_t detail, uint32_t aux) {
    (void)kind;
    (void)pc;
    (void)detail;
    (void)aux;
}
static inline void sr_flight_exit(uint32_t status) {
    (void)status;
}
static inline void sr_flight_budget(uint32_t vblank) {
    (void)vblank;
}
static inline void sr_flight_hang(uint32_t vblanks_without_frame, uint32_t limit) {
    (void)vblanks_without_frame;
    (void)limit;
}
static inline void sr_flight_fault(uint32_t kind, uint32_t pc, uint32_t detail, uint32_t aux) {
    (void)kind;
    (void)pc;
    (void)detail;
    (void)aux;
}
static inline void sr_flight_host_fault(uint32_t guest_pc, uint32_t exception_code,
                                        uint32_t guest_fault_addr) {
    (void)guest_pc;
    (void)exception_code;
    (void)guest_fault_addr;
}
static inline void sr_flight_nan_origin(uint32_t guest_pc, uint32_t domain, uint32_t dst,
                                        uint32_t result, uint32_t lane, uint32_t vbl) {
    (void)guest_pc;
    (void)domain;
    (void)dst;
    (void)result;
    (void)lane;
    (void)vbl;
}
static inline void sr_flight_snapshot(SrFlightSnapshot *out) {
    if (out) *out = (SrFlightSnapshot){0};
}
static inline int sr_flight_event_count(void) { return 0; }
static inline int sr_flight_event_at(uint32_t index, SrFlightEvent *out) {
    (void)index;
    (void)out;
    return 0;
}
static inline void sr_flight_set_thread_source(SrFlightThreadSource source) {
    (void)source;
}
#if defined(SR_HLE_THREAD_SELFTEST) || defined(SR_SCHED_LIVENESS_TEST)
static inline void sr_flight_test_reset(uint32_t enabled_classes, uint32_t limit) {
    (void)enabled_classes;
    (void)limit;
}
static inline void sr_flight_test_disable(void) {}
static inline int sr_flight_test_trigger(const char *spec) {
    (void)spec;
    return 0;
}
#endif
#define SR_FLIGHT_RECORD_CLASS(event_class, kind, arg0, arg1, arg2, arg3) ((void)0)

#endif

#ifdef __cplusplus
}
#endif

#endif
