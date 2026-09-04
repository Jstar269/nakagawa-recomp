// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 the Nakagawa Recomp authors
// PSP threading oracle: CreateThread and StartThread hardware measurements.
// Frozen design from docs/research/PSP_THREADING_SEMANTICS.md.
// Status semantics (Stage 3):
//   PASS = MEASURED: experiment completed structurally and all required
//          measurement fields were captured. Raw PSP API return code remains
//          in `result`; an API error can still be PASS if that was the
//          measured outcome.
//   FAIL = ERROR: harness itself could not produce valid measurement.
//   SKIP = NOT_MEASURABLE_WITH_CURRENT_HARNESS
// Unknown semantics are preserved as raw data, never encoded as expected PASS/FAIL.
// No emulator assumption is treated as hardware fact; every record is raw
// return code plus bounded scalar observations. HARDWARE_NOT_RUN until
// explicit authorized execution.

#include <pspkernel.h>
#include <pspdisplay.h>
#include <psppower.h>
#include <pspthreadman.h>
#include <pspsysmem.h>
#include <psputils.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "thread_shim.h"

PSP_MODULE_INFO("PSP_THREADING_ORACLE", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);

#define CAMPAIGN_VERSION "psp-threading-v1"
#define TEST_ID "PSP-THREAD-001"
#define SCHEMA 1

// Canary for integrity
#define CANARY_VALUE 0xA5A5A5A5u
#define NOT_MEASURED 0xFFFFFFFFu

volatile ThreadEntrySnapshot g_thread_snapshot;
volatile int g_snapshot_valid = 0;

// Forward C handler for shim
int thread_entry_c(int argSize, void *argp);

// Globals for scheduling phased probe
static volatile uint32_t g_phase = 0;
static volatile uint32_t g_entry_count = 0;
static volatile uint32_t g_child_phase_snapshot = 0xFFFFFFFFu;

// Synthetic patterns for StartThread arg copy
static uint8_t g_arg_small[16];
static uint8_t g_arg_boundary[64];

// Stack probe window: 64 bytes
#define STACK_PROBE_BYTES 64

// Phase runners and their helpers are intentionally phase-gated: each CASE
// build uses only its own subset (CASE=all uses all). The attribute keeps
// every build warning-free without masking any other warning class.
#define PHASE_MAYBE_UNUSED __attribute__((unused))

// Helper: bounded checksum (simple xor+ sum)
static PHASE_MAYBE_UNUSED uint32_t checksum_bytes(const uint8_t *buf, uint32_t len) {
    uint32_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (sum * 31u) ^ buf[i];
    return sum;
}

// Checked arithmetic helpers (Stage 7): must check before forming pointers.
static inline PHASE_MAYBE_UNUSED int checked_add_u32(uint32_t base, uint32_t offset, uint32_t *out) {
    if (offset > UINT32_MAX - base) return -1;
    *out = base + offset;
    return 0;
}
static inline PHASE_MAYBE_UNUSED int checked_range_u32(uint32_t base, uint32_t offset, uint32_t length, uint32_t size, uint32_t *out_addr) {
    // Validate offset <= size and length <= size - offset, then base+offset
    if (offset > size) return -1;
    if (length > size - offset) return -1;
    if (checked_add_u32(base, offset, out_addr) != 0) return -1;
    // Also ensure base+offset+length does not overflow and stays within allocation
    uint32_t end;
    if (checked_add_u32(*out_addr, length, &end) != 0) return -1;
    uint32_t alloc_end;
    if (checked_add_u32(base, size, &alloc_end) != 0) return -1;
    if (end > alloc_end) return -1;
    return 0;
}

// Helper: emit meta
static int g_emulated = 0;
static void emit_raw(const char *text) {
    if (g_emulated) {
        sceIoDevctl("emulator:", 2, (void*)text, (int)strlen(text), NULL, 0);
    } else {
        printf("%s", text);
    }
}
static int is_emulator(void) {
    uint32_t flag = 0;
    if (sceIoDevctl("emulator:", 3, NULL, 0, &flag, sizeof(flag)) < 0) return 0;
    return flag == 1;
}
static void emit_meta(uint32_t run_id_low) {
    char line[512];
    snprintf(line, sizeof(line),
        "NAKAGAWA_PSP_META schema=1 source=psp campaign_version=%s run_id=0x%08x model=unknown firmware=unknown binary_sha256=0000000000000000000000000000000000000000000000000000000000000000 source_commit=0000000000000000000000000000000000000000\n",
        CAMPAIGN_VERSION, (unsigned int)run_id_low);
    emit_raw(line);
}
static void emit_test(const char *case_id, const char *status, uint32_t result,
                      const char *extra_fields, uint32_t canary, uint32_t attempt) {
    char line[2048];
    char attempt_field[32];
    snprintf(attempt_field, sizeof(attempt_field), "attempt=0x%08x", (unsigned int)attempt);
    if (extra_fields && extra_fields[0]) {
        snprintf(line, sizeof(line),
            "NAKAGAWA_PSP_TEST schema=1 test_id=%s case_id=%s status=%s result=0x%08x %s %s canary=0x%08x\n",
            TEST_ID, case_id, status, (unsigned int)result, extra_fields, attempt_field, (unsigned int)canary);
    } else {
        snprintf(line, sizeof(line),
            "NAKAGAWA_PSP_TEST schema=1 test_id=%s case_id=%s status=%s result=0x%08x %s canary=0x%08x\n",
            TEST_ID, case_id, status, (unsigned int)result, attempt_field, (unsigned int)canary);
    }
    emit_raw(line);
}

// Forward for generic entries
static PHASE_MAYBE_UNUSED int generic_entry(SceSize args, void *argp) {
    (void)args; (void)argp;
    return 0x42;
}
static PHASE_MAYBE_UNUSED int return_77_entry(SceSize args, void *argp) { (void)args; (void)argp; return 0x77; }
static PHASE_MAYBE_UNUSED int return_negative_entry(SceSize args, void *argp) { (void)args; (void)argp; return (int)0x800201ac; }
static PHASE_MAYBE_UNUSED int return_threshold_entry(SceSize args, void *argp) { (void)args; (void)argp; return (int)0x80000000; }
static PHASE_MAYBE_UNUSED int sleep_entry(SceSize args, void *argp) { (void)args; (void)argp; sceKernelSleepThread(); return 0x55; }

// Helper to get thread status via ReferThreadStatus safely bounded.
// Pre-fills with a sentinel so fields the firmware leaves untouched are
// distinguishable from firmware-written zeros. The buffer is our own
// allocation, so this can never read outside it.
static PHASE_MAYBE_UNUSED int try_refer_status(SceUID thid, SceKernelThreadInfo *info, uint32_t *out_status, uint32_t *out_waittype) {
    memset(info, 0xA5, sizeof(*info));
    info->size = sizeof(*info);
    int ret = sceKernelReferThreadStatus(thid, info);
    if (ret == 0) {
        *out_status = (uint32_t)info->status;
        *out_waittype = (uint32_t)info->waitType;
        return 0;
    }
    *out_status = NOT_MEASURED;
    *out_waittype = NOT_MEASURED;
    return ret;
}

// Stack probe with checked arithmetic and orientation validation (Stage 7).
// Returns 0 on successful probe, 1 on NOT_MEASURABLE (explicitly recorded),
// negative on harness error. Never reads outside allocation. Uses
// info.stack as reported value but validates orientation/extent before use.
// If actual stackSize is unavailable/zero, we record NOT_MEASURABLE instead
// of guessing.
static PHASE_MAYBE_UNUSED int probe_stack_window(SceUID thid, uint32_t *out_sp, uint32_t *out_checksum, uint32_t probe_bytes) {
    SceKernelThreadInfo info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    int ret = sceKernelReferThreadStatus(thid, &info);
    if (ret < 0) return ret;
    uintptr_t stack_addr = (uintptr_t)info.stack;
    // Attempt to recover stackSize. Some SDK revisions expose info.stackSize;
    // if not available or zero, we cannot prove orientation/extent -> NOT_MEASURABLE.
    uint32_t stack_size = 0;
#ifdef PSP_THREAD_INFO_HAS_STACKSIZE
    stack_size = info.stackSize;
#else
    // Try to detect stackSize via sizeof overflow check: if SDK wrote beyond known struct,
    // we cannot safely read without overrunning. So treat as unknown -> NOT_MEASURABLE.
    // Harness only creates stacks of 0x1000..0x40000, but we must not assume.
    // If stack_addr is in user partition and size unknown, we still cannot prove bounds.
    // Record NOT_MEASURABLE_WITH_CURRENT_HARNESS.
    stack_size = 0; // force NOT_MEASURABLE unless SDK explicitly provides
    // For builds where SDK does provide stackSize via extendedstruct, the macro above handles.
#endif
    if (stack_size == 0) {
        *out_sp = (uint32_t)stack_addr;
        *out_checksum = 0; // no probe performed
        return 1; // NOT_MEASURABLE
    }
    // Validate probe window 0x100 .. 0x100+probe_bytes is inside allocation using checked arithmetic
    uint32_t probe_offset = 0x100u;
    uint32_t probe_addr;
    if (checked_range_u32((uint32_t)stack_addr, probe_offset, probe_bytes, stack_size, &probe_addr) != 0) {
        *out_sp = (uint32_t)stack_addr;
        *out_checksum = 0;
        return 1; // window would exceed allocation – safe fail-closed, no read
    }
    // Also ensure probe end is within user partition as sanity (defense in depth)
    // Use checked_add to derive probe_end, never direct probe_addr + probe_bytes
    uint32_t probe_end;
    if (checked_add_u32(probe_addr, probe_bytes, &probe_end) != 0) {
        *out_sp = (uint32_t)stack_addr;
        *out_checksum = 0;
        return 1; // overflow forming probe end
    }
    if (probe_addr < 0x08800000u || probe_end > 0x0C000000u) {
        *out_sp = (uint32_t)stack_addr;
        *out_checksum = 0;
        return 1;
    }
    uint32_t checksum = 0;
    for (uint32_t i = 0; i < probe_bytes; i++) {
        volatile uint8_t *p = (volatile uint8_t*)(uintptr_t)(probe_addr + i);
        checksum = (checksum * 31u) ^ *p;
    }
    *out_sp = (uint32_t)stack_addr;
    *out_checksum = checksum;
    return 0;
}

// Measurement helpers for each class

static PHASE_MAYBE_UNUSED void run_ct_attr_cases(void) {
    struct { const char *id; uint32_t attr; } cases[] = {
        {"CT-A01", 0},
        {"CT-A03", 0x00004000u},
        {"CT-A04", 0x80000000u},
        {"CT-A05", 0xA0000000u},
        {"CT-A07", 0xC0000000u},
        {"CT-A11", 0x00008000u},
        {"CT-A12", 0x00100000u},
    };
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        SceUID thid = sceKernelCreateThread(cases[i].id, generic_entry, 32, 0x1000, cases[i].attr, NULL);
        uint32_t result = (thid < 0) ? (uint32_t)thid : 0;
        uint32_t out_status = NOT_MEASURED, out_wait = NOT_MEASURED;
        uint32_t out_eff = NOT_MEASURED; // never fabricate effective_attr as requested_attr
        uint32_t thread_status = 0, wait_type = 0;
        SceKernelThreadInfo info;
        const char *harness_status = "PASS"; // MEASURED
        if (thid >= 0) {
            int r = try_refer_status(thid, &info, &thread_status, &wait_type);
            out_status = thread_status;
            out_wait = wait_type;
#ifdef PSP_THREAD_INFO_HAS_ATTR
            // If SDK exposes observed attr, use it; otherwise keep NOT_MEASURED
            out_eff = (uint32_t)info.attr;
#else
            out_eff = NOT_MEASURED;
#endif
            (void)r;
            sceKernelDeleteThread(thid);
        } else {
            // Creation failed – still a valid measurement (API error in result), but harness succeeded in capturing it
            // So status remains PASS (MEASURED) with raw result = error code.
            out_status = NOT_MEASURED;
            out_wait = NOT_MEASURED;
        }
        char extra[256];
        snprintf(extra, sizeof(extra), "out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x", (unsigned int)thid, (unsigned int)out_status, (unsigned int)out_wait, (unsigned int)out_eff);
        // For control CT-A01, raw result 0 means success, but PASS still means measurement completed
        emit_test(cases[i].id, harness_status, result, extra, CANARY_VALUE, 0);
        sceKernelDelayThread(1000);
    }
    // CT-D00 stack/status control via thid 0 vs explicit current UID; does NOT prove orientation.
    // Executes before content probing per design requirements. Orientation remains HARDWARE_UNKNOWN.
    // Additional stack-status discriminator: measured stack_reported and stack_orientation from ReferThreadStatus.
    {
        const char *id = "CT-D00";
        SceKernelThreadInfo info;
        int ret = -1;
        uint32_t cur_thid = (uint32_t)sceKernelGetThreadId();
        memset(&info, 0, sizeof(info)); info.size = sizeof(info);
        ret = sceKernelReferThreadStatus(0, &info);
        uint32_t status0 = NOT_MEASURED, wait0 = NOT_MEASURED;
        if (ret == 0) { status0 = info.status; wait0 = info.waitType; }
        SceKernelThreadInfo info2; memset(&info2,0,sizeof(info2)); info2.size=sizeof(info2);
        int ret2 = sceKernelReferThreadStatus((SceUID)cur_thid, &info2);
        uint32_t status2 = NOT_MEASURED, wait2 = NOT_MEASURED;
        if (ret2 == 0) { status2 = info2.status; wait2 = info2.waitType; }
        char extra[512];
        snprintf(extra, sizeof(extra), "out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x out4=0x%08x out5=0x%08x",
            (unsigned int)ret, (unsigned int)status0, (unsigned int)wait0, (unsigned int)ret2, (unsigned int)status2, (unsigned int)wait2);
        // Probe stack window for current thid using safe helper – orientation establishing
        if (ret2 == 0) {
            uint32_t probe_sp = 0, probe_csum = 0;
            int pr = probe_stack_window((SceUID)cur_thid, &probe_sp, &probe_csum, STACK_PROBE_BYTES);
            if (pr == 0) {
                char more[128];
                snprintf(more, sizeof(more), " out6=0x%08x out7=0x%08x", (unsigned int)probe_sp, (unsigned int)probe_csum);
                strncat(extra, more, sizeof(extra)-strlen(extra)-1);
            } else if (pr == 1) {
                char more[128];
                snprintf(more, sizeof(more), " out6=0x%08x out7=0x%08x", (unsigned int)probe_sp, NOT_MEASURED);
                strncat(extra, more, sizeof(extra)-strlen(extra)-1);
            } else {
                char more[128];
                snprintf(more, sizeof(more), " out6=0x%08x out7=0x%08x", NOT_MEASURED, NOT_MEASURED);
                strncat(extra, more, sizeof(extra)-strlen(extra)-1);
            }
        } else {
            char more[128];
            snprintf(more, sizeof(more), " out6=0x%08x out7=0x%08x", NOT_MEASURED, NOT_MEASURED);
            strncat(extra, more, sizeof(extra)-strlen(extra)-1);
        }
        emit_test(id, "PASS", (uint32_t)ret, extra, CANARY_VALUE, 0);
    }
}

static PHASE_MAYBE_UNUSED void run_ct_prio_opt_cases(void) {
    struct { const char *id; int prio; uint32_t stack; } prio_cases[] = {
        {"CT-B01", 8, 0x1000},
        {"CT-B02", 120, 0x1000},
        {"CT-B05", 16, 512},
        {"CT-B06", 16, 262144},
    };
    for (size_t i=0;i<sizeof(prio_cases)/sizeof(prio_cases[0]);i++) {
        SceUID thid = sceKernelCreateThread(prio_cases[i].id, generic_entry, prio_cases[i].prio, prio_cases[i].stack, 0, NULL);
        uint32_t result = (thid < 0) ? (uint32_t)thid : 0;
        char extra[128];
        snprintf(extra, sizeof(extra), "out0=0x%08x out1=0x%08x", (unsigned int)thid, (unsigned int)prio_cases[i].stack);
        emit_test(prio_cases[i].id, "PASS", result, extra, CANARY_VALUE, 0);
        if (thid >= 0) sceKernelDeleteThread(thid);
        sceKernelDelayThread(500);
    }
    // Option structure cases CT-C01..C04 – gating minimum (FULL: C05/C06 excluded from 28-case minimum)
    // CT-C01 NULL
    {
        SceUID thid = sceKernelCreateThread("CT-C01", generic_entry, 32, 0x1000, 0, NULL);
        uint32_t result = (thid < 0)?(uint32_t)thid:0;
        char extra[128]; snprintf(extra,sizeof(extra),"out0=0x%08x", (unsigned int)thid);
        emit_test("CT-C01", "PASS", result, extra, CANARY_VALUE, 0);
        if (thid>=0) sceKernelDeleteThread(thid);
    }
    // CT-C02 size=4
    {
        SceKernelThreadOptParam opt; memset(&opt,0,sizeof(opt)); opt.size=4;
        SceUID thid = sceKernelCreateThread("CT-C02", generic_entry, 32, 0x1000, 0, &opt);
        uint32_t result = (thid < 0)?(uint32_t)thid:0;
        char extra[128]; snprintf(extra,sizeof(extra),"out0=0x%08x", (unsigned int)thid);
        emit_test("CT-C02", "PASS", result, extra, CANARY_VALUE, 0);
        if (thid>=0) sceKernelDeleteThread(thid);
    }
    // CT-C03 size=8 stackMpid=USER(2) – primary partition hypothesis
    {
        SceKernelThreadOptParam opt; memset(&opt,0,sizeof(opt)); opt.size=8; opt.stackMpid=2;
        SceUID thid = sceKernelCreateThread("CT-C03", generic_entry, 32, 0x1000, 0, &opt);
        uint32_t result = (thid < 0)?(uint32_t)thid:0;
        char extra[128]; snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x", (unsigned int)thid, 2u);
        emit_test("CT-C03", "PASS", result, extra, CANARY_VALUE, 0);
        if (thid>=0) sceKernelDeleteThread(thid);
    }
    // CT-C04 size=8 invalid 7
    {
        SceKernelThreadOptParam opt; memset(&opt,0,sizeof(opt)); opt.size=8; opt.stackMpid=7;
        SceUID thid = sceKernelCreateThread("CT-C04", generic_entry, 32, 0x1000, 0, &opt);
        uint32_t result = (thid < 0)?(uint32_t)thid:0;
        char extra[128]; snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x", (unsigned int)thid, 7u);
        emit_test("CT-C04", "PASS", result, extra, CANARY_VALUE, 0);
        if (thid>=0) sceKernelDeleteThread(thid);
    }
    // Extended CT-C05 (kernel 1) and CT-C06 (genuine block UID) belong to the
    // full 59/71 campaign (lower-risk beyond the 28-case gating minimum).
    // They are available for manual full-campaign builds via PSP_THREADING_FULL=1
    // and are not emitted in the 5-launch gating build so that the record count
    // matches matrix.json (28/56). See fixtures/psp_threading_oracle/README.md.
#ifdef PSP_THREADING_FULL
    // CT-C05 size=8 stackMpid=KERNEL(1) – kernel partition discriminator (FULL only)
    {
        SceKernelThreadOptParam opt; memset(&opt,0,sizeof(opt)); opt.size=8; opt.stackMpid=1;
        SceUID thid = sceKernelCreateThread("CT-C05", generic_entry, 32, 0x1000, 0, &opt);
        uint32_t result = (thid < 0)?(uint32_t)thid:0;
        char extra[128]; snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x", (unsigned int)thid, 1u);
        emit_test("CT-C05", "PASS", result, extra, CANARY_VALUE, 0);
        if (thid>=0) sceKernelDeleteThread(thid);
    }
    // CT-C06 genuine block UID – competing PSPSDK hypothesis (FULL only)
    {
        SceUID block = sceKernelAllocPartitionMemory(2, "oracle-c06", PSP_SMEM_Low, 4096, NULL);
        uint32_t block_uid = (block < 0) ? NOT_MEASURED : (uint32_t)block;
        uint32_t block_ret = (block < 0) ? (uint32_t)block : 0;
        SceUID thid = -1;
        uint32_t result = 0;
        const char *harness_status = "PASS";
        if (block >= 0) {
            SceKernelThreadOptParam opt; memset(&opt,0,sizeof(opt)); opt.size=8; opt.stackMpid= (SceUID)block_uid;
            thid = sceKernelCreateThread("CT-C06", generic_entry, 32, 0x1000, 0, &opt);
            result = (thid < 0)?(uint32_t)thid:0;
            if (thid < 0) {
                // Creation failed but we still have valid measurement of attempt – still PASS
                result = (uint32_t)thid;
            }
        } else {
            // Allocation failed – record as harness-level measurement, not silent
            // So we emit PASS with block_ret indicating alloc failure, thid = error sentinel
            thid = -1;
            result = block_ret;
            harness_status = "PASS"; // allocation failure is measured data
        }
        char extra[256];
        snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x out2=0x%08x", (unsigned int)thid, (unsigned int)block_uid, (unsigned int)block_ret);
        emit_test("CT-C06", harness_status, result, extra, CANARY_VALUE, 0);
        if (thid>=0) sceKernelDeleteThread(thid);
        if (block>=0) sceKernelFreePartitionMemory(block);
    }
#endif
}

static PHASE_MAYBE_UNUSED void run_st_args_cases(void) {
    for (int i=0;i<16;i++) g_arg_small[i] = (uint8_t)(0x11u + (i*0x22u));
    for (int i=0;i<64;i++) g_arg_boundary[i] = (uint8_t)(i & 0xFF);
    // ST-SA01 zero arg – must never dereference child arg pointer (Stage 8)
    {
        SceUID thid = sceKernelCreateThread("ST-SA01", thread_entry_shim, 32, 0x2000, 0, NULL);
        uint32_t result = 0;
        uint32_t child_a0=NOT_MEASURED, child_a1=NOT_MEASURED, child_sp=NOT_MEASURED, child_gp=NOT_MEASURED, child_ra=NOT_MEASURED;
        uint32_t canary = NOT_MEASURED;
        const char *harness_status = "PASS";
        if (thid >= 0) {
            g_snapshot_valid = 0; memset((void*)&g_thread_snapshot,0,sizeof(g_thread_snapshot));
            g_child_phase_snapshot = NOT_MEASURED;
            g_entry_count=0;
            result = (uint32_t)sceKernelStartThread(thid, 0, NULL);
            // Deterministic synchronization: wait for child to exit instead of delay
            sceKernelWaitThreadEnd(thid, NULL);
            if (g_snapshot_valid) { child_a0=g_thread_snapshot.a0; child_a1=g_thread_snapshot.a1; child_sp=g_thread_snapshot.sp; child_gp=g_thread_snapshot.gp; child_ra=g_thread_snapshot.ra; canary=g_thread_snapshot.canary; }
            sceKernelDeleteThread(thid);
        } else {
            result = (uint32_t)thid;
            harness_status = "FAIL"; // harness could not create thread
        }
        char extra[512];
        snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x out4=0x%08x out5=0x%08x", (unsigned int)child_a0,(unsigned int)child_a1,(unsigned int)child_sp,(unsigned int)child_gp,(unsigned int)child_ra,(unsigned int)canary);
        emit_test("ST-SA01", harness_status, result, extra, CANARY_VALUE, 0);
    }
    // ST-SA02 small 16 – preserve raw source/child checksums, pointers, inside, delta without mutation
    {
        SceUID thid = sceKernelCreateThread("ST-SA02", thread_entry_shim, 32, 0x2000, 0, NULL);
        if (thid>=0) {
            g_snapshot_valid=0; memset((void*)&g_thread_snapshot,0,sizeof(g_thread_snapshot));
            g_child_phase_snapshot = NOT_MEASURED;
            uint32_t src_csum = checksum_bytes(g_arg_small,16);
            uint32_t src_ptr = (uint32_t)(uintptr_t)g_arg_small;
            // Pre-capture stack base for later inside check
            SceKernelThreadInfo info; memset(&info,0,sizeof(info)); info.size=sizeof(info);
            sceKernelReferThreadStatus(thid,&info);
            uintptr_t stack_low_pre = (uintptr_t)info.stack;
            (void)stack_low_pre;
            uint32_t result = (uint32_t)sceKernelStartThread(thid, 16, g_arg_small);
            // Deterministic wait
            sceKernelWaitThreadEnd(thid, NULL);
            uint32_t child_a0=NOT_MEASURED, child_a1=NOT_MEASURED, child_sp=NOT_MEASURED;
            uint32_t child_csum=NOT_MEASURED, inside=0, diff=NOT_MEASURED;
            uint32_t raw_eq=0;
            if (g_snapshot_valid) {
                child_a0=g_thread_snapshot.a0; child_a1=g_thread_snapshot.a1; child_sp=g_thread_snapshot.sp;
                // Check inside using checked arithmetic against reported stack allocation
                SceKernelThreadInfo info2; memset(&info2,0,sizeof(info2)); info2.size=sizeof(info2);
                int r2 = sceKernelReferThreadStatus(thid,&info2);
                uint32_t stack_low = 0, stack_size = 0;
                if (r2==0) {
                    stack_low = (uint32_t)(uintptr_t)info2.stack;
#ifdef PSP_THREAD_INFO_HAS_STACKSIZE
                    stack_size = info2.stackSize;
#else
                    stack_size = 0;
#endif
                }
                // Use checked range to prove inside before any memory read (Stage 7)
                if (stack_size != 0 && child_a1 != 0) {
                    uint32_t probe_addr;
                    if (checked_range_u32(stack_low, child_a1 - stack_low, 16, stack_size, &probe_addr)==0) {
                        inside=1;
                    } else {
                        // Try heuristic: if stack_low==0 (no size), mark as NOT_MEASURABLE (inside=0 but not proof)
                        inside=0;
                    }
                } else {
                    inside=0;
                    // Without stack_size we cannot prove inside – mark 0 and do not read
                }
                // Preserve raw child checksum without mutation
                if (inside && child_a1 != 0) {
                    // Use bounded copy with checked arithmetic already validated
                    uint8_t tmp[16];
                    // Safe because we validated inside range; still use memcpy bounded
                    memcpy(tmp,(void*)(uintptr_t)child_a1,16);
                    child_csum = checksum_bytes(tmp,16);
                } else if (child_a1 != 0) {
                    // Not provably inside – do not read, mark NOT_MEASURED per Stage 7
                    child_csum = NOT_MEASURED;
                    inside = 0;
                } else {
                    child_csum = NOT_MEASURED;
                }
                raw_eq = (src_csum == child_csum && child_csum != NOT_MEASURED) ? 1 : 0;
                // Do NOT mutate checksum to signal mismatch – preserve raw
                if (child_sp != 0 && child_a1 != 0) {
                    if (child_a1 >= child_sp) diff = child_a1 - child_sp;
                    else diff = NOT_MEASURED;
                }
            }
            char extra2[1024];
            snprintf(extra2,sizeof(extra2),"out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x out4=0x%08x out5=0x%08x out6=0x%08x out7=0x%08x out8=0x%08x",
                (unsigned int)child_a0,(unsigned int)child_a1,(unsigned int)child_sp,
                (unsigned int)src_csum,(unsigned int)child_csum,(unsigned int)raw_eq,
                (unsigned int)src_ptr,(unsigned int)inside,(unsigned int)diff);
            emit_test("ST-SA02", "PASS", result, extra2, CANARY_VALUE, 0);
            sceKernelDeleteThread(thid);
        } else {
            emit_test("ST-SA02", "FAIL", (uint32_t)thid, "out0=0x00000000 out1=0x00000000 out2=0x00000000 out3=0x00000000 out4=0x00000000 out5=0x00000000 out6=0x00000000 out7=0x00000000 out8=0x00000000", CANARY_VALUE, 0);
        }
    }
    // ST-SA03 boundary 64
    {
        SceUID thid = sceKernelCreateThread("ST-SA03", thread_entry_shim, 32, 0x4000, 0, NULL);
        if (thid>=0) {
            g_snapshot_valid=0; memset((void*)&g_thread_snapshot,0,sizeof(g_thread_snapshot));
            g_child_phase_snapshot = NOT_MEASURED;
            uint32_t src_csum = checksum_bytes(g_arg_boundary,64);
            uint32_t src_ptr = (uint32_t)(uintptr_t)g_arg_boundary;
            uint32_t result = (uint32_t)sceKernelStartThread(thid, 64, g_arg_boundary);
            sceKernelWaitThreadEnd(thid, NULL);
            uint32_t child_a0=NOT_MEASURED, child_a1=NOT_MEASURED, child_sp=NOT_MEASURED;
            uint32_t child_csum=NOT_MEASURED, inside=0, diff=NOT_MEASURED, raw_eq=0;
            if (g_snapshot_valid) {
                child_a0=g_thread_snapshot.a0; child_a1=g_thread_snapshot.a1; child_sp=g_thread_snapshot.sp;
                SceKernelThreadInfo info; memset(&info,0,sizeof(info)); info.size=sizeof(info); sceKernelReferThreadStatus(thid,&info);
                uint32_t stack_low = (uint32_t)(uintptr_t)info.stack;
                uint32_t stack_size = 0;
#ifdef PSP_THREAD_INFO_HAS_STACKSIZE
                stack_size = info.stackSize;
#else
                stack_size = 0;
#endif
                if (stack_size != 0 && child_a1 != 0) {
                    uint32_t probe_addr;
                    if (checked_range_u32(stack_low, child_a1 - stack_low, 64, stack_size, &probe_addr)==0) {
                        inside=1;
                    }
                }
                if (inside && child_a1) {
                    uint8_t tmp[64]; memcpy(tmp,(void*)(uintptr_t)child_a1,64);
                    child_csum=checksum_bytes(tmp,64);
                } else if (child_a1) {
                    child_csum = NOT_MEASURED;
                    inside=0;
                }
                raw_eq = (src_csum == child_csum && child_csum != NOT_MEASURED) ? 1 : 0;
                if (child_sp !=0 && child_a1!=0 && child_a1 >= child_sp) diff = child_a1 - child_sp;
                else diff = NOT_MEASURED;
                uint32_t child_ptr = child_a1;
                char extra[1024];
                snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x out2=0x%08x out3=0x%08x out4=0x%08x out5=0x%08x out6=0x%08x out7=0x%08x out8=0x%08x",
                    (unsigned int)child_a0,(unsigned int)child_ptr,(unsigned int)child_sp,
                    (unsigned int)src_csum,(unsigned int)child_csum,(unsigned int)raw_eq,
                    (unsigned int)src_ptr,(unsigned int)inside,(unsigned int)diff);
                emit_test("ST-SA03", "PASS", result, extra, CANARY_VALUE, 0);
            } else {
                emit_test("ST-SA03", "PASS", result, "out0=0x00000000 out1=0x00000000 out2=0x00000000 out3=0x00000000 out4=0x00000000 out5=0x00000000 out6=0x00000000 out7=0x00000000 out8=0x00000000", CANARY_VALUE, 0);
            }
            sceKernelDeleteThread(thid);
        } else {
            emit_test("ST-SA03", "FAIL", (uint32_t)thid, "out0=0x00000000 out1=0x00000000 out2=0x00000000 out3=0x00000000 out4=0x00000000 out5=0x00000000 out6=0x00000000 out7=0x00000000 out8=0x00000000", CANARY_VALUE, 0);
        }
    }
}

static PHASE_MAYBE_UNUSED void run_st_life_ret_cases(void) {
    // ST-SL01 start dormant -> should succeed
    {
        SceUID thid = sceKernelCreateThread("ST-SL01", generic_entry, 32, 0x1000, 0, NULL);
        uint32_t result=0, out0=0;
        if (thid>=0) {
            result=(uint32_t)sceKernelStartThread(thid,0,NULL);
            // deterministic wait
            sceKernelWaitThreadEnd(thid, NULL);
            out0=(result==0)?1:0;
            sceKernelDeleteThread(thid);
        } else { result=(uint32_t)thid; out0=0; }
        char extra[128]; snprintf(extra,sizeof(extra),"out0=0x%08x", (unsigned int)out0);
        emit_test("ST-SL01", "PASS", result, extra, CANARY_VALUE, 0);
    }
    // ST-SL02 start active -> should error (raw code preserved)
    {
        SceUID thid = sceKernelCreateThread("ST-SL02", sleep_entry, 32, 0x1000, 0, NULL);
        uint32_t result=0;
        if (thid>=0) {
            sceKernelStartThread(thid,0,NULL);
            // Wait briefly for child to enter sleep state via WaitThreadEnd with timeout? Alternative: yield
            sceKernelDelayThread(1000); // minimal yield to let child sleep, but not used as proof
            result=(uint32_t)sceKernelStartThread(thid,0,NULL);
            // Clean up: terminate then delete
            sceKernelTerminateDeleteThread(thid);
        } else result=(uint32_t)thid;
        char extra[128]; snprintf(extra,sizeof(extra),"out0=0x%08x", (unsigned int)(result!=0));
        emit_test("ST-SL02", "PASS", result, extra, CANARY_VALUE, 0);
    }
    // ST-SL04 start exited (restart) -> should restart from entry
    {
        SceUID thid = sceKernelCreateThread("ST-SL04", return_77_entry, 32, 0x1000, 0, NULL);
        uint32_t result=0, cnt_after=0;
        if (thid>=0) {
            sceKernelStartThread(thid,0,NULL);
            sceKernelWaitThreadEnd(thid,NULL); // first run exits
            g_entry_count=0;
            result=(uint32_t)sceKernelStartThread(thid,0,NULL);
            sceKernelWaitThreadEnd(thid,NULL);
            cnt_after=g_entry_count;
            sceKernelDeleteThread(thid);
        } else result=(uint32_t)thid;
        char extra[128]; snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x", (unsigned int)cnt_after, (unsigned int)(result==0));
        emit_test("ST-SL04", "PASS", result, extra, CANARY_VALUE, 0);
    }
    // ST-SR01 return 0x77 -> wait should get 0x77
    {
        SceUID thid = sceKernelCreateThread("ST-SR01", return_77_entry, 32, 0x1000, 0, NULL);
        uint32_t wait_ret=NOT_MEASURED, exit_status=NOT_MEASURED;
        uint32_t start_ret=0;
        if (thid>=0) {
            start_ret=(uint32_t)sceKernelStartThread(thid,0,NULL);
            int wait = sceKernelWaitThreadEnd(thid,NULL);
            wait_ret=(uint32_t)wait;
            SceKernelThreadInfo info; memset(&info,0,sizeof(info)); info.size=sizeof(info);
            if(sceKernelReferThreadStatus(thid,&info)==0) exit_status=info.exitStatus;
            sceKernelDeleteThread(thid);
        }
        char extra[256]; snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x out2=0x%08x", (unsigned int)wait_ret,(unsigned int)exit_status,(unsigned int)start_ret);
        emit_test("ST-SR01", "PASS", start_ret, extra, CANARY_VALUE, 0);
    }
    // ST-SR02 return 0x800201ac -> normalized to 0x800200d2 ?
    {
        SceUID thid = sceKernelCreateThread("ST-SR02", return_negative_entry, 32, 0x1000, 0, NULL);
        uint32_t wait_ret=NOT_MEASURED, exit_status=NOT_MEASURED, start_ret=0;
        if (thid>=0) {
            start_ret=(uint32_t)sceKernelStartThread(thid,0,NULL);
            int wait=sceKernelWaitThreadEnd(thid,NULL);
            wait_ret=(uint32_t)wait;
            SceKernelThreadInfo info; memset(&info,0,sizeof(info)); info.size=sizeof(info);
            if(sceKernelReferThreadStatus(thid,&info)==0) exit_status=info.exitStatus;
            sceKernelDeleteThread(thid);
        }
        char extra[256]; snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x out2=0x%08x", (unsigned int)wait_ret,(unsigned int)exit_status,(unsigned int)start_ret);
        emit_test("ST-SR02", "PASS", start_ret, extra, CANARY_VALUE, 0);
    }
    // ST-SR03 threshold 0x80000000
    {
        SceUID thid = sceKernelCreateThread("ST-SR03", return_threshold_entry, 32, 0x1000, 0, NULL);
        uint32_t wait_ret=NOT_MEASURED, exit_status=NOT_MEASURED, start_ret=0;
        if (thid>=0) {
            start_ret=(uint32_t)sceKernelStartThread(thid,0,NULL);
            int wait=sceKernelWaitThreadEnd(thid,NULL);
            wait_ret=(uint32_t)wait;
            SceKernelThreadInfo info; memset(&info,0,sizeof(info)); info.size=sizeof(info);
            if(sceKernelReferThreadStatus(thid,&info)==0) exit_status=info.exitStatus;
            sceKernelDeleteThread(thid);
        }
        char extra[256]; snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x out2=0x%08x", (unsigned int)wait_ret,(unsigned int)exit_status,(unsigned int)start_ret);
        emit_test("ST-SR03", "PASS", start_ret, extra, CANARY_VALUE, 0);
    }
}

static PHASE_MAYBE_UNUSED void run_st_sched_cases(void) {
    struct { const char *id; int child_prio; int ctrl_prio; } sched[] = {
        {"ST-SP01", 16, 32},
        {"ST-SP02", 32, 32},
        {"ST-SP03", 48, 32},
    };
    for (size_t i=0;i<sizeof(sched)/sizeof(sched[0]);i++) {
        sceKernelChangeThreadPriority(0, sched[i].ctrl_prio);
        SceUID thid = sceKernelCreateThread(sched[i].id, thread_entry_shim, sched[i].child_prio, 0x2000, 0, NULL);
        uint32_t result=0; uint32_t phase_snap=NOT_MEASURED, entry_cnt=NOT_MEASURED;
        if (thid>=0) {
            g_phase=1; g_entry_count=0; g_snapshot_valid=0; memset((void*)&g_thread_snapshot,0,sizeof(g_thread_snapshot));
            g_child_phase_snapshot = NOT_MEASURED;
            result=(uint32_t)sceKernelStartThread(thid,0,NULL);
            g_phase=2;
            // Deterministic wait for child to run (instead of arbitrary delay)
            sceKernelWaitThreadEnd(thid, NULL);
            phase_snap = g_child_phase_snapshot;
            entry_cnt = g_entry_count;
            sceKernelDeleteThread(thid);
        } else result=(uint32_t)thid;
        char extra[256]; snprintf(extra,sizeof(extra),"out0=0x%08x out1=0x%08x out2=0x%08x", (unsigned int)phase_snap,(unsigned int)entry_cnt,(unsigned int)sched[i].child_prio);
        emit_test(sched[i].id, "PASS", result, extra, CANARY_VALUE, 0);
        sceKernelDelayThread(500);
    }
    sceKernelChangeThreadPriority(0, 32);
}

int thread_entry_c(int argSize, void *argp) {
    (void)argSize; (void)argp;
    g_child_phase_snapshot = g_phase;
    g_entry_count++;
    return 0x77;
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;
    g_emulated = is_emulator();
    pspDebugScreenInit();
    uint32_t run_id = (uint32_t)sceKernelGetSystemTimeLow();
    emit_meta(run_id);

#if PSP_THREADING_CASE == 100 // all
    run_ct_attr_cases();
    run_ct_prio_opt_cases();
    run_st_args_cases();
    run_st_life_ret_cases();
    run_st_sched_cases();
#elif PSP_THREADING_CASE == 101
    run_ct_attr_cases();
#elif PSP_THREADING_CASE == 102
    run_ct_prio_opt_cases();
#elif PSP_THREADING_CASE == 103
    run_st_args_cases();
#elif PSP_THREADING_CASE == 104
    run_st_life_ret_cases();
#elif PSP_THREADING_CASE == 105
    run_st_sched_cases();
#else
    run_ct_attr_cases();
#endif

    sceKernelDcacheWritebackAll();
    sceKernelExitGame();
    return 0;
}
