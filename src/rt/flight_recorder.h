// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

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
    SR_FLIGHT_CLASS_ALL = (1u << 6) - 1u,
    SR_FLIGHT_MAX_EVENTS = 4096u,
    SR_FLIGHT_SCHEMA_VERSION = 1u
};

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
    SR_FLIGHT_KIND_FATAL_HOST = 17u
};

enum {
    SR_FLIGHT_TERMINAL_RUNNING = 0u,
    SR_FLIGHT_TERMINAL_EXIT = 1u,
    SR_FLIGHT_TERMINAL_FATAL = 2u,
    SR_FLIGHT_TERMINAL_UNSUPPORTED_NID = 3u
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
} SrFlightSnapshot;

#if defined(SR_FLIGHT_RECORDER_LINKED) && !defined(SR_FLIGHT_RECORDER_STANDALONE)

void sr_flight_init(void);
int sr_flight_class_enabled(uint32_t event_class);
void sr_flight_record(uint32_t event_class, uint32_t kind, uint32_t arg0, uint32_t arg1,
                      uint32_t arg2, uint32_t arg3);
void sr_flight_hle_import(uint32_t nid, uint32_t uid, uint32_t object_uid, uint32_t pc,
                          uint32_t ra);
void sr_flight_unsupported(uint32_t nid, uint32_t error, uint32_t uid, uint32_t pc);
void sr_flight_unsupported_fatal(uint32_t nid, uint32_t uid, uint32_t pc);
void sr_flight_prx_load(uint32_t base, uint32_t result, uint32_t entry, uint32_t imports,
                        uint32_t exports);
void sr_flight_fatal(uint32_t kind, uint32_t pc, uint32_t detail, uint32_t aux);
void sr_flight_exit(uint32_t status);
void sr_flight_fault(uint32_t kind, uint32_t pc, uint32_t detail, uint32_t aux);
void sr_flight_snapshot(SrFlightSnapshot *out);
int sr_flight_event_count(void);
int sr_flight_event_at(uint32_t index, SrFlightEvent *out);

#if defined(SR_HLE_THREAD_SELFTEST)
void sr_flight_test_reset(uint32_t enabled_classes, uint32_t limit);
void sr_flight_test_disable(void);
#endif

#define SR_FLIGHT_RECORD_CLASS(event_class, kind, arg0, arg1, arg2, arg3)                         \
    do {                                                                                          \
        if (sr_flight_class_enabled((event_class))) {                                            \
            sr_flight_record((event_class), (kind), (arg0), (arg1), (arg2), (arg3));             \
        }                                                                                         \
    } while (0)

#else

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
static inline void sr_flight_hle_import(uint32_t nid, uint32_t uid, uint32_t object_uid, uint32_t pc,
                                        uint32_t ra) {
    (void)nid;
    (void)uid;
    (void)object_uid;
    (void)pc;
    (void)ra;
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
static inline void sr_flight_fault(uint32_t kind, uint32_t pc, uint32_t detail, uint32_t aux) {
    (void)kind;
    (void)pc;
    (void)detail;
    (void)aux;
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
#if defined(SR_HLE_THREAD_SELFTEST)
static inline void sr_flight_test_reset(uint32_t enabled_classes, uint32_t limit) {
    (void)enabled_classes;
    (void)limit;
}
static inline void sr_flight_test_disable(void) {}
#endif
#define SR_FLIGHT_RECORD_CLASS(event_class, kind, arg0, arg1, arg2, arg3) ((void)0)

#endif

#ifdef __cplusplus
}
#endif

#endif
