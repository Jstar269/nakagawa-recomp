// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2025-2026 the psp-recomp authors

#ifndef SR_FLIGHT_RECORDER_LINKED
#define SR_FLIGHT_RECORDER_LINKED
#endif
#include "flight_recorder.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__clang__)
#define SR_FLIGHT_COMPILER "clang"
#elif defined(__GNUC__)
#define SR_FLIGHT_COMPILER "gcc"
#else
#define SR_FLIGHT_COMPILER "other"
#endif

typedef struct {
    uint32_t mask;
    const char *name;
} SrFlightClassName;

/* ---- SR_TRACE_PC: the address window over the instruction trace ----
 * The contract is documented in flight_recorder.h. The stream is opened here so
 * this diagnostic does not depend on the trace writer being linked, which every
 * host of the runtime links instead. */
static FILE *s_tw_fp;
static int s_tw_configured;
static int s_tw_armed;
static int s_tw_skip;
static uint32_t s_tw_lo, s_tw_hi;
static SrTraceWindowIndices s_tw_indices;
static unsigned long s_tw_limit, s_tw_count;

static void sr_trace_window_list(const char *text, uint8_t *out, unsigned *count) {
    *count = 0;
    if (!text || !text[0]) return;
    while (*text && *count < SR_TRACE_WINDOW_MAX_INDEX) {
        char *end = NULL;
        const unsigned long value = strtoul(text, &end, 0);
        if (end == text) return;
        if (value < 128u) out[(*count)++] = (uint8_t)value;
        text = end;
        while (*text == ',' || *text == ' ') text++;
    }
}

void sr_trace_window_configure(void) {
    if (s_tw_configured) return;
    s_tw_configured = 1;
    const char *window = getenv("SR_TRACE_PC");
    if (!window || !window[0]) return;
    const char *path = getenv("SR_TRACE");
    if (!path || !path[0]) path = "logs/trace_window.txt";
    const char *sep = strchr(window, ':');
    if (!sep) sep = strchr(window, '-');
    if (!sep) {
        fprintf(stderr, "TRACE_WINDOW: SR_TRACE_PC needs LO:HI, got '%s'\n", window);
        return;
    }
    const uint32_t lo = (uint32_t)strtoul(window, NULL, 0);
    const uint32_t hi = (uint32_t)strtoul(sep + 1, NULL, 0);
    if (hi < lo) {
        fprintf(stderr, "TRACE_WINDOW: empty window %s\n", window);
        return;
    }
    sr_trace_window_list(getenv("SR_TRACE_V"), s_tw_indices.v, &s_tw_indices.vn);
    sr_trace_window_list(getenv("SR_TRACE_F"), s_tw_indices.f, &s_tw_indices.fn);
    const char *limit = getenv("SR_TRACE_LIMIT");
    s_tw_limit = limit && limit[0] ? strtoul(limit, NULL, 0) : 0ul;
    s_tw_fp = fopen(path, "wb");
    if (!s_tw_fp) {
        fprintf(stderr, "TRACE_WINDOW: cannot open '%s'\n", path);
        return;
    }
    fprintf(s_tw_fp, "# trace-window v1 lo=0x%08x hi=0x%08x limit=%lu v=%u f=%u\n",
            lo, hi, s_tw_limit, s_tw_indices.vn, s_tw_indices.fn);
    s_tw_lo = lo;
    s_tw_hi = hi;
    s_tw_armed = 1;
    fprintf(stderr, "TRACE_WINDOW: armed lo=0x%08x hi=0x%08x limit=%lu v=%u f=%u path=%s\n",
            lo, hi, s_tw_limit, s_tw_indices.vn, s_tw_indices.fn, path);
}

void sr_trace_note_frame(uint32_t frame) {
    if (!s_tw_armed || !s_tw_fp) return;
    fprintf(s_tw_fp, "frame %u\n", frame);
}

int sr_trace_window_armed(void) { return s_tw_armed; }

int sr_trace_window_begin_instruction(uint32_t pc) {
    s_tw_skip = pc < s_tw_lo || pc > s_tw_hi;
    return !s_tw_skip;
}

const SrTraceWindowIndices *sr_trace_window_indices(void) {
    return s_tw_armed ? &s_tw_indices : 0;
}

void sr_trace_window_record(uint32_t pc, const char *text) {
    (void)pc;
    if (!s_tw_armed || s_tw_skip || !s_tw_fp) return;
    fprintf(s_tw_fp, "%s\n", text);
    if (s_tw_limit && ++s_tw_count >= s_tw_limit) {
        fprintf(s_tw_fp, "# window complete records=%lu\n", s_tw_count);
        fflush(s_tw_fp);
        /* Disarm rather than close: the trace writer's own stream is untouched,
         * and the gate stops admitting in-window instructions, so the rest of
         * the run executes uninstrumented. */
        s_tw_armed = 0;
    }
}

static SrFlightEvent s_flight_events[SR_FLIGHT_MAX_EVENTS];
static uint32_t s_flight_classes;
static uint32_t s_flight_limit = 256u;
static uint64_t s_flight_recorded;
static uint64_t s_flight_last_sequence;
static uint32_t s_flight_last_kind;
static int s_flight_init_state;
static int s_flight_exit_registered;
static int s_flight_dumped;
static uint32_t s_flight_dump_count;
static uint64_t s_flight_trigger_count;
static uint32_t s_flight_terminal_reason;
static uint32_t s_flight_terminal_kind;
static uint32_t s_flight_terminal_arg;
static uint64_t s_flight_terminal_sequence;

static const SrFlightClassName s_flight_class_names[] = {
    {SR_FLIGHT_CLASS_HLE, "hle"},
    {SR_FLIGHT_CLASS_UNSUPPORTED, "unsupported"},
    {SR_FLIGHT_CLASS_SCHED, "sched"},
    {SR_FLIGHT_CLASS_PRX, "prx"},
    {SR_FLIGHT_CLASS_FAULT, "fault"},
    {SR_FLIGHT_CLASS_FATAL, "fatal"},
};

static int token_is(const char *text, size_t length, const char *name) {
    return strlen(name) == length && memcmp(text, name, length) == 0;
}

static int parse_limit(const char *text, uint32_t *limit_out) {
    uint32_t value = 0u;
    if (!text || !*text) return 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p < '0' || *p > '9') return 0;
        if (value > (SR_FLIGHT_MAX_EVENTS - (uint32_t)(*p - '0')) / 10u) return 0;
        value = value * 10u + (uint32_t)(*p - '0');
    }
    if (value == 0u || value > SR_FLIGHT_MAX_EVENTS) return 0;
    *limit_out = value;
    return 1;
}

static int parse_classes(const char *text, size_t length, uint32_t *mask_out) {
    uint32_t mask = 0u;
    size_t start = 0u;
    if (length == 0u) return 0;
    for (size_t i = 0u; i <= length; ++i) {
        int matched = 0;
        if (i != length && text[i] != ',') continue;
        if (i == start) return 0;
        for (size_t j = 0u; j < sizeof(s_flight_class_names) / sizeof(s_flight_class_names[0]); ++j) {
            if (token_is(text + start, i - start, s_flight_class_names[j].name)) {
                mask |= s_flight_class_names[j].mask;
                matched = 1;
                break;
            }
        }
        if (!matched) return 0;
        if (i == length) {
            if (mask == 0u) return 0;
            *mask_out = mask;
            return 1;
        }
        start = i + 1u;
    }
    return 0;
}

static const char *class_name(uint32_t event_class) {
    for (size_t i = 0u; i < sizeof(s_flight_class_names) / sizeof(s_flight_class_names[0]); ++i) {
        if (s_flight_class_names[i].mask == event_class) return s_flight_class_names[i].name;
    }
    return "unknown";
}

static int write_bundle(int reason);
static void flight_exit(void);

static void register_exit(void) {
    if (s_flight_exit_registered || s_flight_classes == 0u) return;
    if (atexit(flight_exit) != 0) {
        fprintf(stderr, "SR_FLIGHT: could not register exit bundle; recorder disabled\n");
        s_flight_classes = 0u;
        return;
    }
    s_flight_exit_registered = 1;
}

void sr_flight_init(void) {
    if (s_flight_init_state == 2) return;
    if (s_flight_init_state == 1) return;
    s_flight_init_state = 1;

    const char *spec = getenv("SR_FLIGHT");
    if (spec && *spec) {
        const char *separator = strchr(spec, ';');
        size_t class_length = separator ? (size_t)(separator - spec) : 0u;
        uint32_t classes = 0u;
        uint32_t limit = 0u;
        if (!separator || !parse_classes(spec, class_length, &classes) ||
            !parse_limit(separator + 1, &limit)) {
            fprintf(stderr, "SR_FLIGHT: expected named classes and limit (for example hle,sched;256); "
                            "recorder disabled\n");
        } else {
            s_flight_classes = classes;
            s_flight_limit = limit;
            register_exit();
        }
    }
    s_flight_init_state = 2;
}

int sr_flight_class_enabled(uint32_t event_class) {
    return (s_flight_classes & event_class) != 0u;
}

static uint64_t record_locked(uint32_t event_class, uint32_t kind, uint32_t arg0, uint32_t arg1,
                              uint32_t arg2, uint32_t arg3) {
    if (s_flight_classes == 0u || s_flight_limit == 0u || s_flight_trigger_count != 0u) return 0u;
    if (s_flight_recorded == UINT64_MAX) return 0u;
    uint64_t sequence = s_flight_recorded + 1u;
    uint32_t slot = (uint32_t)((sequence - 1u) % s_flight_limit);
    s_flight_events[slot] = (SrFlightEvent){
        SR_FLIGHT_SCHEMA_VERSION,
        sequence,
        event_class,
        kind,
        arg0,
        arg1,
        arg2,
        arg3,
        {0u, 0u, 0u, 0u},
        0u,
        0u,
    };
    s_flight_recorded = sequence;
    s_flight_last_sequence = sequence;
    s_flight_last_kind = kind;
    return sequence;
}

void sr_flight_record(uint32_t event_class, uint32_t kind, uint32_t arg0, uint32_t arg1,
                      uint32_t arg2, uint32_t arg3) {
    if (!sr_flight_class_enabled(event_class)) return;
    (void)record_locked(event_class, kind, arg0, arg1, arg2, arg3);
}

uint64_t sr_flight_hle_import(uint32_t nid, uint32_t uid, uint32_t object_uid, uint32_t pc,
                              uint32_t ra) {
    uint32_t classes = s_flight_classes;
    uint64_t sequence = 0u;
    if (classes & SR_FLIGHT_CLASS_HLE) {
        sequence = record_locked(SR_FLIGHT_CLASS_HLE, SR_FLIGHT_KIND_HLE_IMPORT, nid, uid, pc, ra);
    }
    if (!(classes & SR_FLIGHT_CLASS_PRX)) return sequence;
    uint32_t kind = 0u;
    switch (nid) {
    case 0x977de386u: kind = SR_FLIGHT_KIND_PRX_LOAD; object_uid = 0u; break;
    case 0x50f0c1ecu: kind = SR_FLIGHT_KIND_PRX_START; break;
    case 0xd1ff982au: kind = SR_FLIGHT_KIND_PRX_STOP; break;
    case 0x2e0911aau: kind = SR_FLIGHT_KIND_PRX_UNLOAD; break;
    default: return sequence;
    }
    (void)record_locked(SR_FLIGHT_CLASS_PRX, kind, object_uid, pc, 0u, 0u);
    return sequence;
}

static SrFlightEvent *event_for_sequence(uint64_t sequence) {
    if (sequence == 0u || s_flight_limit == 0u || sequence > s_flight_recorded) return NULL;
    uint64_t first = s_flight_recorded > s_flight_limit ? s_flight_recorded - s_flight_limit : 0u;
    if (sequence <= first) return NULL;
    uint32_t slot = (uint32_t)((sequence - 1u) % s_flight_limit);
    SrFlightEvent *event = &s_flight_events[slot];
    return event->sequence == sequence && event->event_class == SR_FLIGHT_CLASS_HLE &&
                   event->kind == SR_FLIGHT_KIND_HLE_IMPORT
               ? event
               : NULL;
}

void sr_flight_hle_arguments(uint64_t sequence, uint32_t arg0, uint32_t arg1, uint32_t arg2,
                             uint32_t arg3) {
    if (!(s_flight_classes & SR_FLIGHT_CLASS_HLE)) return;
    SrFlightEvent *event = event_for_sequence(sequence);
    if (!event) return;
    event->arguments[0] = arg0;
    event->arguments[1] = arg1;
    event->arguments[2] = arg2;
    event->arguments[3] = arg3;
}

void sr_flight_hle_return(uint64_t sequence, uint32_t return_value) {
    if (!(s_flight_classes & SR_FLIGHT_CLASS_HLE)) return;
    SrFlightEvent *event = event_for_sequence(sequence);
    if (!event) return;
    event->has_return = 1u;
    event->return_value = return_value;
}

static void trigger(int reason, uint32_t kind, uint32_t arg) {
    if (s_flight_trigger_count != 0u) return;
    s_flight_trigger_count = 1u;
    s_flight_terminal_reason = (uint32_t)reason;
    s_flight_terminal_kind = kind;
    s_flight_terminal_arg = arg;
    s_flight_terminal_sequence = s_flight_last_sequence;
    (void)write_bundle(reason);
}

void sr_flight_unsupported(uint32_t nid, uint32_t error, uint32_t uid, uint32_t pc) {
    if (s_flight_classes == 0u) return;
    if (s_flight_classes & SR_FLIGHT_CLASS_UNSUPPORTED) {
        record_locked(SR_FLIGHT_CLASS_UNSUPPORTED, SR_FLIGHT_KIND_UNSUPPORTED_NID, nid, error, uid, pc);
    }
    trigger(SR_FLIGHT_TERMINAL_UNSUPPORTED_NID, SR_FLIGHT_KIND_UNSUPPORTED_NID, nid);
}

void sr_flight_unsupported_fatal(uint32_t nid, uint32_t uid, uint32_t pc) {
    uint32_t classes = s_flight_classes;
    if (classes == 0u) return;
    if (classes & SR_FLIGHT_CLASS_UNSUPPORTED) {
        record_locked(SR_FLIGHT_CLASS_UNSUPPORTED, SR_FLIGHT_KIND_UNSUPPORTED_NID, nid, 0u, uid, pc);
    }
    if (classes & SR_FLIGHT_CLASS_FATAL) {
        record_locked(SR_FLIGHT_CLASS_FATAL, SR_FLIGHT_KIND_FATAL_DISPATCH, pc, nid, uid, 0u);
    }
    trigger((classes & SR_FLIGHT_CLASS_UNSUPPORTED) ? SR_FLIGHT_TERMINAL_UNSUPPORTED_NID
                                                    : SR_FLIGHT_TERMINAL_FATAL,
            SR_FLIGHT_KIND_FATAL_DISPATCH, nid);
}

void sr_flight_prx_load(uint32_t base, uint32_t result, uint32_t entry, uint32_t imports,
                        uint32_t exports) {
    if (!sr_flight_class_enabled(SR_FLIGHT_CLASS_PRX)) return;
    record_locked(SR_FLIGHT_CLASS_PRX, SR_FLIGHT_KIND_PRX_LOAD, base, result, entry,
                  (imports & 0xffffu) | ((exports & 0xffffu) << 16));
}

void sr_flight_fatal(uint32_t kind, uint32_t pc, uint32_t detail, uint32_t aux) {
    if (s_flight_classes == 0u) return;
    if (s_flight_classes & SR_FLIGHT_CLASS_FATAL) {
        record_locked(SR_FLIGHT_CLASS_FATAL, kind, pc, detail, aux, 0u);
    }
    trigger(SR_FLIGHT_TERMINAL_FATAL, kind, detail);
}

void sr_flight_exit(uint32_t status) {
    if (s_flight_classes == 0u || s_flight_dumped) return;
    s_flight_terminal_reason = SR_FLIGHT_TERMINAL_EXIT;
    s_flight_terminal_kind = 0u;
    s_flight_terminal_arg = status;
    s_flight_terminal_sequence = s_flight_last_sequence;
    (void)write_bundle(SR_FLIGHT_TERMINAL_EXIT);
}

void sr_flight_fault(uint32_t kind, uint32_t pc, uint32_t detail, uint32_t aux) {
    SR_FLIGHT_RECORD_CLASS(SR_FLIGHT_CLASS_FAULT, kind, pc, detail, aux, 0u);
}

static uint32_t retained_count(void) {
    if (s_flight_recorded < s_flight_limit) return (uint32_t)s_flight_recorded;
    return s_flight_limit;
}

void sr_flight_snapshot(SrFlightSnapshot *out) {
    if (!out) return;
    if (s_flight_init_state != 2) sr_flight_init();
    uint32_t retained = retained_count();
    *out = (SrFlightSnapshot){
        s_flight_classes,
        s_flight_limit,
        retained,
        s_flight_recorded,
        s_flight_recorded - retained,
        s_flight_trigger_count,
        s_flight_dump_count,
        s_flight_terminal_reason,
        s_flight_terminal_kind,
        s_flight_terminal_arg,
        s_flight_terminal_sequence,
    };
}

int sr_flight_event_count(void) {
    if (s_flight_init_state != 2) sr_flight_init();
    return (int)retained_count();
}

int sr_flight_event_at(uint32_t index, SrFlightEvent *out) {
    if (!out || s_flight_init_state != 2) return 0;
    uint32_t count = retained_count();
    if (index >= count) return 0;
    uint64_t first = s_flight_recorded > s_flight_limit ? s_flight_recorded - s_flight_limit : 0u;
    uint32_t slot = (uint32_t)((first + index) % s_flight_limit);
    *out = s_flight_events[slot];
    return 1;
}

static int write_enabled_classes(FILE *file, uint32_t classes) {
    int wrote = 0;
    if (fputc('[', file) == EOF) return 0;
    for (size_t i = 0u; i < sizeof(s_flight_class_names) / sizeof(s_flight_class_names[0]); ++i) {
        if (!(classes & s_flight_class_names[i].mask)) continue;
        if (wrote && fputc(',', file) == EOF) return 0;
        if (fprintf(file, "\"%s\"", s_flight_class_names[i].name) < 0) return 0;
        wrote = 1;
    }
    return fputc(']', file) != EOF;
}

static int write_bundle(int reason) {
    if (s_flight_dumped || s_flight_classes == 0u) return 0;
    const char *output = getenv("SR_FLIGHT_OUTPUT");
    if (!output || !*output) output = "flight-recorder.json";
    char temporary[512];
    int length = snprintf(temporary, sizeof(temporary), "%s.tmp", output);
    if (length < 0 || (size_t)length >= sizeof(temporary)) {
        fprintf(stderr, "SR_FLIGHT: output path is too long; bundle not written\n");
        return 0;
    }

    FILE *file = fopen(temporary, "wb");
    if (!file) {
        fprintf(stderr, "SR_FLIGHT: could not open evidence output; bundle not written\n");
        return 0;
    }
    int ok = 1;
    ok = ok && fprintf(file, "{\n  \"schema_version\": %u,\n", SR_FLIGHT_SCHEMA_VERSION) >= 0;
    ok = ok && fprintf(file, "  \"runtime\": {\"name\": \"nakagawa-recomp\", \"cpu_state_abi\": 2},\n") >= 0;
    ok = ok && fprintf(file,
                       "  \"build\": {\"compiler\": \"%s\", \"compiled_date\": \"%s\", "
                       "\"compiled_time\": \"%s\", \"pointer_bits\": %u},\n",
                       SR_FLIGHT_COMPILER, __DATE__, __TIME__, (unsigned)(sizeof(void *) * 8u)) >= 0;
    ok = ok && fputs("  \"recorder\": {\"enabled_classes\": ", file) != EOF;
    ok = ok && write_enabled_classes(file, s_flight_classes);
    ok = ok && fprintf(file,
                       ", \"limit\": %u, \"recorded\": %llu, \"dropped\": %llu, "
                       "\"triggers\": {\"first_fatal\": %s, \"first_unsupported_nid\": %s, "
                       "\"fired\": %llu}},\n",
                        s_flight_limit, (unsigned long long)s_flight_recorded,
                        (unsigned long long)(s_flight_recorded - retained_count()),
                        "true", "true", (unsigned long long)s_flight_trigger_count) >= 0;
    const char *terminal = reason == SR_FLIGHT_TERMINAL_FATAL ? "fatal" :
                           reason == SR_FLIGHT_TERMINAL_UNSUPPORTED_NID ? "unsupported-nid" :
                           reason == SR_FLIGHT_TERMINAL_EXIT ? "exit" : "running";
    ok = ok && fprintf(file,
                       "  \"terminal\": {\"reason\": \"%s\", \"sequence\": %llu, "
                       "\"kind\": %u, \"arg0\": %u},\n",
                       terminal, (unsigned long long)s_flight_terminal_sequence,
                       s_flight_terminal_kind, s_flight_terminal_arg) >= 0;
    ok = ok && fputs("  \"events\": [", file) != EOF;
    uint32_t count = retained_count();
    uint64_t first = s_flight_recorded > s_flight_limit ? s_flight_recorded - s_flight_limit : 0u;
    for (uint32_t i = 0u; ok && i < count; ++i) {
        uint32_t slot = (uint32_t)((first + i) % s_flight_limit);
        const SrFlightEvent *event = &s_flight_events[slot];
        ok = fprintf(file,
                     "%s    {\"schema_version\": %u, \"sequence\": %llu, \"class\": \"%s\", "
                     "\"kind\": %u, \"arg0\": %u, \"arg1\": %u, \"arg2\": %u, \"arg3\": %u",
                     i ? ",\n" : "\n", event->schema_version, (unsigned long long)event->sequence,
                     class_name(event->event_class), event->kind, event->arg0, event->arg1,
                     event->arg2, event->arg3) >= 0;
        if (ok && event->event_class == SR_FLIGHT_CLASS_HLE &&
            event->kind == SR_FLIGHT_KIND_HLE_IMPORT) {
            ok = fprintf(file, ", \"arguments\": [%u, %u, %u, %u], \"return_value\": ",
                         event->arguments[0], event->arguments[1], event->arguments[2],
                         event->arguments[3]) >= 0;
            if (ok) {
                if (event->has_return) {
                    ok = fprintf(file, "%u", event->return_value) >= 0;
                } else {
                    ok = fputs("null", file) != EOF;
                }
            }
        }
        ok = ok && fputc('}', file) != EOF;
    }
    ok = ok && fputs(count ? "\n  ]\n}\n" : "]\n}\n", file) != EOF;
    int file_error = ferror(file);
    int close_error = fclose(file);
    if (file_error || close_error) ok = 0;
    if (!ok) {
        remove(temporary);
        fprintf(stderr, "SR_FLIGHT: evidence output failed; bundle not written\n");
        return 0;
    }
    if (remove(output) != 0 && errno != ENOENT) {
        remove(temporary);
        fprintf(stderr, "SR_FLIGHT: could not replace evidence output; bundle not written\n");
        return 0;
    }
    if (rename(temporary, output) != 0) {
        remove(temporary);
        fprintf(stderr, "SR_FLIGHT: could not publish evidence output; bundle not written\n");
        return 0;
    }
    s_flight_dumped = 1;
    s_flight_dump_count++;
    return 1;
}

static void flight_exit(void) {
    if (s_flight_classes == 0u || s_flight_dumped) return;
    if (s_flight_trigger_count == 0u) {
        s_flight_terminal_reason = SR_FLIGHT_TERMINAL_EXIT;
        s_flight_terminal_kind = 0u;
        s_flight_terminal_arg = 0u;
        s_flight_terminal_sequence = s_flight_last_sequence;
    }
    (void)write_bundle((int)s_flight_terminal_reason);
}

#if defined(SR_HLE_THREAD_SELFTEST)
void sr_flight_test_reset(uint32_t enabled_classes, uint32_t limit) {
    s_flight_classes = enabled_classes & SR_FLIGHT_CLASS_ALL;
    s_flight_limit = limit == 0u ? 1u : (limit > SR_FLIGHT_MAX_EVENTS ? SR_FLIGHT_MAX_EVENTS : limit);
    memset(s_flight_events, 0, sizeof(s_flight_events));
    s_flight_recorded = 0u;
    s_flight_last_sequence = 0u;
    s_flight_last_kind = 0u;
    s_flight_init_state = 2;
    s_flight_dumped = 0;
    s_flight_dump_count = 0u;
    s_flight_trigger_count = 0u;
    s_flight_terminal_reason = SR_FLIGHT_TERMINAL_RUNNING;
    s_flight_terminal_kind = 0u;
    s_flight_terminal_arg = 0u;
    s_flight_terminal_sequence = 0u;
    register_exit();
}

void sr_flight_test_disable(void) {
    s_flight_classes = 0u;
    s_flight_init_state = 2;
}
#endif
